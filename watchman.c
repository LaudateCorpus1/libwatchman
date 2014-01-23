#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "watchman.h"
#include <jansson.h>

static void watchman_err(watchman_error_t *error,
			  char *message, ...)
{
	va_list argptr;
	va_start(argptr, message);
	char c;
	int len = vsnprintf(&c, 1, message, argptr);
	va_end(argptr);

	error->message = malloc(len + 1);
	va_start(argptr, message);
	vsnprintf(error->message, len + 1, message, argptr);
	va_end(argptr);
}

watchman_connection_t *watchman_sock_connect(watchman_error_t *error, const char* sockname)
{
	struct sockaddr_un addr = {};

	int fd;
	if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		watchman_err(error, "socket error %d", errno);
		return NULL;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockname, sizeof(addr.sun_path)-1);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		watchman_err(error, "connect error %d", errno);
		return NULL;
	}

	FILE *sockfp = fdopen(fd, "r+");
	if (!sockfp) {
		watchman_err(error, "Failed to connect to watchman socket %s.",
			      sockname);
		return NULL;
	}
	setlinebuf(sockfp);

	watchman_connection_t *conn = malloc(sizeof(watchman_connection_t));
	conn->fp = sockfp;
	return conn;
}

watchman_connection_t *watchman_connect(watchman_error_t *error)
{
	watchman_connection_t *conn = NULL;
	FILE *fp = popen("watchman get-sockname", "r");
	if (!fp) {
		watchman_err(error,
			     "Could not watchman get-sockname: %d",
			errno);
		return NULL;
	}
	json_error_t jerror;
	json_t *json = json_loadf(fp, 0, &jerror);
	fclose(fp);
	if (!json) {
		watchman_err(error,
			     "Got bad JSON from watchman get-sockname: %s",
			     jerror.text);
		goto done;
	}
	if (!json_is_object(json)) {
		watchman_err(error, "Got bad JSON from watchman get-sockname:"
			       " object expected");
		goto bad_json;
	}
	json_t *sockname_obj = json_object_get(json, "sockname");
	if (!sockname_obj) {
		watchman_err(error, "Got bad JSON from watchman get-sockname:"
			     " socket expected");
		goto bad_json;
	}
	if (!json_is_string(sockname_obj)) {
		watchman_err(error, "Got bad JSON from watchman get-sockname:"
			     " socket is not string");
		goto bad_json;
	}
	const char *sockname = json_string_value(sockname_obj);
	conn = watchman_sock_connect(error, sockname);
bad_json:
	json_decref(json);
done:
	return conn;
}

static int watchman_send_simple_cmd(watchman_connection_t *conn,
				    watchman_error_t *error,
				    ...)
{

	int result = 0;
	json_t *cmd_array = json_array();
	va_list argptr;
	va_start(argptr, error);
	char *arg;
	while (arg = va_arg(argptr, char*)) {
		json_array_append_new(cmd_array, json_string(arg));
	}
	int json_result = json_dumpf(cmd_array, conn->fp, JSON_COMPACT);
	if (json_result) {
		watchman_err(error, "Failed to send simple watchman command");
		result = 1;
	}
	fputc('\n', conn->fp);

	json_decref(cmd_array);
	return result;
}

static json_t *watchman_read(
	watchman_connection_t *conn,
	watchman_error_t *error
	)
{
	json_error_t jerror;
	int flags = JSON_DISABLE_EOF_CHECK;
	json_t *result = json_loadf(conn->fp, flags, &jerror);
	if (!result) {
		watchman_err(error, "Got unparseable or empty result from watchman: %s", jerror.text);
	}
	if (fgetc(conn->fp) != '\n') {
		watchman_err(error, "No newline at end of reply");
		json_decref(result);
		result = NULL;
	}
	return result;
}

static int watchman_read_and_handle_errors(
	watchman_connection_t *conn,
	watchman_error_t *error) {

	json_t *obj = watchman_read(conn, error);
	if (!obj) {
		return 1;
	}
	if (!json_is_object(obj)) {
		char *bogus_json_text = json_dumps(obj, 0);
		watchman_err(error, "Got non-object result from watchman : %s",
			     bogus_json_text);
		free(bogus_json_text);
		json_decref(obj);
		return 1;
	}
	json_t *error_json = json_object_get(obj, "error");
	if (error_json) {
		watchman_err(error, "Got error result from watchman : %s",
			     json_string_value(error_json));
		json_decref(obj);
		return 1;
	}

	json_decref(obj);
	return 0;
}

int watchman_watch(
	watchman_connection_t *conn,
	const char *path,
	watchman_error_t *error)
{
	if (watchman_send_simple_cmd(conn, error, "watch", path, NULL)) {
		return 1;
	}
	if (watchman_read_and_handle_errors(conn, error)) {
		return 1;
	}
	return 0;
}

int watchman_watch_del(
	watchman_connection_t *conn,
	const char *path,
	watchman_error_t *error)
{
	if (watchman_send_simple_cmd(conn, error, "watch-del", path, NULL)) {
		return 1;
	}
	if (watchman_read_and_handle_errors(conn, error)) {
		return 1;
	}
	return 0;
}

static watchman_expression_t *alloc_expr(
	enum watchman_expression_type ty)
{
	watchman_expression_t *expr = calloc(1, sizeof(watchman_expression_t));
	expr->ty = ty;
	return expr;
}

watchman_expression_t * watchman_since_expression(
	const char *since,
	enum watchman_clockspec spec)
{
	assert(since);
	watchman_expression_t *expr = alloc_expr(WATCHMAN_EXPR_TY_SINCE);
	expr->e.since_expr.is_time_t = 0;
	expr->e.since_expr.t.since = strdup(since);
	expr->e.since_expr.clockspec = spec;
	return expr;
}

watchman_expression_t * watchman_since_expression_time_t(
	time_t time,
	enum watchman_clockspec spec)
{
	watchman_expression_t *expr = alloc_expr(WATCHMAN_EXPR_TY_SINCE);
	expr->e.since_expr.is_time_t = 1;
	expr->e.since_expr.t.time = time;
	expr->e.since_expr.clockspec = spec;
	return expr;
}

static char *ty_str [] = {
	"allof",
	"anyof",
	"not",
	"true",
	"false",
	"since",
	"suffix",
	"match",
	"imatch",
	"pcre",
	"ipcre",
	"name",
	"iname",
	"type",
	"empty",
	"exists"
};

static char *clockspec_str[] = {
	NULL,
	"oclock",
	"mtime",
	"ctime"
};

static char *basename_str[] = {
	NULL,
	"basename",
	"wholename"
};

static json_t *json_string_from_char(char c)
{
	char str[2];
	str[0] = c;
	str[1] = 0;
	return json_string(str);
}

static json_t *json_string_or_array(int nr, char** items)
{
	if (nr == 1) {
		return json_string(items[0]);
	}
	json_t *result = json_array();
	int i;
	for (i = 0; i < nr; ++i) {
		json_array_append_new(result, json_string(items[i]));
	}
	return result;
	
}

static json_t *since_to_json(json_t *result, const watchman_expression_t *expr) 
{
	if (expr->e.since_expr.is_time_t) {
		json_array_append_new(result,
				      json_integer(expr->e.since_expr.t.time));
	} else {
		json_array_append_new(result,
				      json_string(expr->e.since_expr.t.since));
	}
	if (expr->e.since_expr.clockspec) {
		char *clockspec = ty_str[expr->e.since_expr.clockspec];
		json_array_append_new(result, json_string(clockspec));
	}
}
static json_t *to_json(const watchman_expression_t *expr)
{
	json_t *result = json_array();
	json_t *arg;
	json_array_append_new(result, json_string(ty_str[expr->ty]));

	watchman_expression_t **clause;
	int i;
	switch(expr->ty) {
	case WATCHMAN_EXPR_TY_ALLOF: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_ANYOF:
		clause = expr->e.union_expr.clauses;
		for (i = 0; i < expr->e.union_expr.nr; ++i, ++clause) {
			json_array_append_new(result, to_json(*clause));
		}
		break;
	case WATCHMAN_EXPR_TY_NOT:
		json_array_append_new(result, to_json(
					      expr->e.not_expr.clause));
		break;
	case WATCHMAN_EXPR_TY_TRUE: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_FALSE: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_EMPTY: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_EXISTS:
		/* Nothing to do */
		break;

	case WATCHMAN_EXPR_TY_SINCE:
		since_to_json(result, expr);

		break;
	case WATCHMAN_EXPR_TY_SUFFIX:
		json_array_append_new(result,
				      json_string(expr->e.suffix_expr.suffix));
		break;
	case WATCHMAN_EXPR_TY_MATCH: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_IMATCH: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_PCRE: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_IPCRE: 
		json_array_append_new(result,
				      json_string(expr->e.match_expr.match));
		if (expr->e.match_expr.basename) {
			char *base = basename_str[expr->e.match_expr.basename];
			json_array_append_new(result, json_string(base));
		}
		break;
	case WATCHMAN_EXPR_TY_NAME: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_INAME:
		arg = json_string_or_array(expr->e.name_expr.nr,
			expr->e.name_expr.names);
		json_array_append_new(result, arg);
		if (expr->e.match_expr.basename) {
			char *base = basename_str[expr->e.match_expr.basename];
			json_array_append_new(result, json_string(base));
		}
		break;
	case WATCHMAN_EXPR_TY_TYPE:
		json_array_append_new(
			result, json_string_from_char(expr->e.type_expr.type));
	}
	return result;
}

static char* fields_str[] = {
	"name",
	"exists",
	"cclock",
	"oclock",
	"ctime",
	"ctime_ms",
	"ctime_us",
	"ctime_ns",
	"ctime_f",
	"mtime",
	"mtime_ms",
	"mtime_us",
	"mtime_ns",
	"mtime_f",
	"size",
	"uid",
	"gid",
	"ino",
	"dev",
	"nlink",
	"new"
};

json_t *fields_to_json(int fields)
{
	json_t *result = json_array();
	int i = 0;
	int mask;
	for (mask = 1; mask <= WATCHMAN_FIELD_NEWER; mask *= 2) {
		if (fields & mask) {
			json_array_append_new(result,
					      json_string(fields_str[i]));
		}
		++i;
	}
	return result;
}

#define JSON_ASSERT(cond, condarg, msg) \
	if (!cond(condarg)) {						\
		char* dump = json_dumps(condarg, 0);			\
		watchman_err(error, msg,				\
			     dump);					\
		free(dump);						\
		goto done;						\
	}

watchman_watch_list_t *watchman_watch_list(watchman_connection_t *conn,
					   watchman_error_t *error)
{

	watchman_watch_list_t *res = NULL;
	if (watchman_send_simple_cmd(conn, error, "watch-list", NULL)) {
		return NULL;
	}

	json_t *obj = watchman_read(conn, error);
	if (!obj) {
		return NULL;
	}
	JSON_ASSERT(json_is_object, obj, "Got bogus value from watch-list %s");
	json_t* roots = json_object_get(obj, "roots");
	JSON_ASSERT(json_is_array, roots, "Got bogus value from watch-list %s");

	res = malloc(sizeof (watchman_watch_list_t));
	res->nr = json_array_size(roots);
	res->roots = calloc(res->nr, sizeof(char *));
	int i;
	for (i = 0; i < res->nr; ++i) {
		json_t *root = json_array_get(roots, i);
		JSON_ASSERT(json_is_string, root,
			    "Got non-string root from watch-list %s");
		res->roots[i] = strdup(json_string_value(root));
	}

done:
	json_decref(obj);
	return res;
}

static watchman_query_result_t *watchman_do_query(
	watchman_connection_t *conn,
	json_t *query,
	watchman_error_t *error)
{

	watchman_query_result_t* result = NULL;
	watchman_query_result_t* res = NULL;

	int json_result = json_dumpf(query, conn->fp, JSON_COMPACT);
	if (json_result) {
		char* dump = json_dumps(query, 0);
		watchman_err(error, "Failed to send watchman query %s",
			     dump);
		free(dump);
		return NULL;
	}
	fputc('\n', conn->fp);
	/* parse the result */
	json_t *obj = watchman_read(conn, error);
	if (!obj) {
		return NULL;
	}
	JSON_ASSERT(json_is_object, obj, "Failed to send watchman query %s");

	res = calloc(1, sizeof(watchman_query_result_t));

	json_t *files = json_object_get(obj, "files");
	JSON_ASSERT(json_is_array, files, "Bad files %s");

	res->nr = json_array_size(files);
	res->stats = calloc(res->nr, sizeof(watchman_stat_t));

	int i;
	for (i = 0; i < res->nr; ++i) {
		watchman_stat_t* stat = res->stats + i;
		json_t *statobj = json_array_get(files, i);
		if (json_is_string(statobj)) {
			/* then hopefully we only requested names */
			stat->name = strdup(json_string_value(statobj));
			continue;
		}

		JSON_ASSERT(json_is_object, statobj, "must be object: %s");

		json_t *name = json_object_get(statobj, "name");
		JSON_ASSERT(json_is_string, name, "name must be string: %s");
		stat->name = strdup(json_string_value(name));

		json_t *exists = json_object_get(statobj, "exists");
		if (exists) {
			stat->exists = json_is_true(exists);
		}

		json_t *mode = json_object_get(statobj, "mode");
		if (mode) {
			stat->mode = json_integer_value(mode);
		}

		json_t *newer = json_object_get(statobj, "new");
		if (newer) {
			stat->newer = json_is_true(newer);
		}

		json_t *size = json_object_get(statobj, "size");
		if (size) {
			stat->newer = json_integer_value(size);
		}
	}

	json_t *version = json_object_get(obj, "version");
	JSON_ASSERT(json_is_string, version, "Bad version %s");
	res->version = strdup(json_string_value(version));

	json_t *clock = json_object_get(obj, "clock");
	JSON_ASSERT(json_is_string, clock, "Bad clock %s");
	res->clock = strdup(json_string_value(clock));

	json_t *fresh = json_object_get(obj, "is_fresh_instance");
	JSON_ASSERT(json_is_boolean, fresh, "Bad is_fresh_instance %s");
	res->is_fresh_instance = json_is_true(fresh);

	result = res;
	goto good;
done:
	if (res) {
		watchman_free_query_result(res);
	}
good:
	json_decref(obj);
	return result;
}

watchman_query_result_t *watchman_query(watchman_connection_t *conn,
				      const char *fs_path,
				      const watchman_expression_t *expr,
				      int fields,
				      watchman_error_t *error)
{

	/* construct the json */
	json_t *query = json_array();
	json_array_append_new(query, json_string("query"));
	json_array_append_new(query, json_string(fs_path));
	json_t *obj = json_object();
	json_object_set_new(obj, "expression", to_json(expr));
	json_object_set_new(obj, "fields", fields_to_json(fields));
	json_array_append_new(query, obj);

	/* do the query */
	watchman_query_result_t *r = watchman_do_query(conn, query, error);
	json_decref(query);
	return r;
}

void watchman_free_expression(watchman_expression_t *expr)
{
	int i;
	switch(expr->ty) {
	case WATCHMAN_EXPR_TY_ALLOF: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_ANYOF:
		for (i = 0; i < expr->e.union_expr.nr; ++i) {
			watchman_free_expression(expr->e.union_expr.clauses[i]);
		}
		free(expr->e.union_expr.clauses);
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_NOT:
		watchman_free_expression(expr->e.not_expr.clause);
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_TRUE: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_FALSE: /*-fallthrough*/
		/* These are singletons; don't delete them */
		break;
	case WATCHMAN_EXPR_TY_SINCE:
		if (!expr->e.since_expr.is_time_t) {
			free(expr->e.since_expr.t.since);
		}
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_SUFFIX:
		free(expr->e.suffix_expr.suffix);
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_MATCH: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_IMATCH: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_PCRE: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_IPCRE: /*-fallthrough*/
		free(expr->e.match_expr.match);
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_NAME: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_INAME:
		for (i = 0; i < expr->e.name_expr.nr; ++i) {
			free(expr->e.name_expr.names[i]);
		}
		free(expr->e.name_expr.names);
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_TYPE:
		free(expr);
		break;
	case WATCHMAN_EXPR_TY_EMPTY: /*-fallthrough*/
	case WATCHMAN_EXPR_TY_EXISTS:
		/* These are singletons; don't delete them */
		break;
	}
}

void watchman_connection_close(watchman_connection_t *conn)
{
	if (!conn->fp) {
		return;
	}
	fclose(conn->fp);
	conn->fp = NULL;
	free(conn);
}

void watchman_free_error(watchman_error_t *error)
{
	if (error->message)
		free(error->message);
	free(error);
}

void watchman_free_watch_list(watchman_watch_list_t *list)
{
	int i;
	for (i = 0; i < list->nr; ++i) {
		free(list->roots[i]);
	}
	free(list->roots);
	free(list);
}

static void watchman_dealloc_stat(watchman_stat_t *stat)
{
	if (stat->name) {
		free(stat->name);
	}
}

void watchman_free_query_result(watchman_query_result_t *result)
{
	if (result->version) {
		free(result->version);
	}
	if (result->clock) {
		free(result->clock);
	}

	if (result->stats) {
		int i;
		for (i = 0; i < result->nr; ++i) {
			watchman_dealloc_stat(&(result->stats[i]));
		}
		free(result->stats);
	}
	free(result);
}

watchman_expression_t* watchman_not_expression(
	watchman_expression_t* expression)
{
	return ;
}

#define UNION_EXPR(tyupper, tylower) \
	watchman_expression_t* watchman_##tylower##_expression(		\
		int nr, watchman_expression_t** expressions) {		\
		assert(nr);						\
		assert(expressions);					\
		size_t sz = sizeof(watchman_expression_t);		\
		watchman_expression_t* result = malloc (sz);		\
		result->ty = WATCHMAN_EXPR_TY_##tyupper;		\
		result->e.union_expr.nr = nr;				\
		result->e.union_expr.clauses = malloc(nr * sz);		\
		memcpy(result->e.union_expr.clauses, expressions, nr * sz); \
		return result;						\
}

UNION_EXPR(ALLOF, allof)
UNION_EXPR(ANYOF, anyof)
#undef UNION_EXPR

#define STATIC_EXPR(ty, tylower)					\
	static watchman_expression_t ty##_EXPRESSION =			\
	{ WATCHMAN_EXPR_TY_##ty };					\
	watchman_expression_t* watchman_##tylower##_expression()	\
	{								\
		return &ty##_EXPRESSION;				\
	} 

STATIC_EXPR(EMPTY, empty)
STATIC_EXPR(TRUE, true)
STATIC_EXPR(FALSE, false)
STATIC_EXPR(EXISTS, exists)

#undef static_expr

watchman_expression_t* watchman_suffix_expression(const char *suffix) {
	assert(suffix);
	watchman_expression_t *expr = alloc_expr(WATCHMAN_EXPR_TY_SUFFIX);
	expr->e.suffix_expr.suffix = strdup(suffix);
	return expr;
}

#define MATCH_EXPR(tyupper, tylower)					\
	watchman_expression_t* watchman_##tylower##_expression		\
	(const char *match, enum watchman_basename basename)		\
	{								\
		assert(match);						\
		watchman_expression_t *expr =				\
			alloc_expr(WATCHMAN_EXPR_TY_##tyupper);		\
		expr->e.match_expr.match = strdup(match);		\
		expr->e.match_expr.basename = basename;			\
		return expr;						\
}

MATCH_EXPR(MATCH, match)
MATCH_EXPR(IMATCH, imatch)
MATCH_EXPR(PCRE, pcre)
MATCH_EXPR(IPCRE, ipcre)

#undef MATCH_EXPR

#define NAME_EXPR(tyupper, tylower)					\
	watchman_expression_t* watchman_##tylower##_expression(		\
		const char *name, enum watchman_basename basename)	\
	{								\
	assert(name);							\
	return watchman_##tylower##s_expression(1, &name, basename);	\
	}
	
NAME_EXPR(NAME, name)
NAME_EXPR(INAME, iname)
#undef NAME_EXPR


#define NAMES_EXPR(tyupper, tylower)					\
	watchman_expression_t* watchman_##tylower##s_expression(	\
		int nr, const char **names,				\
		enum watchman_basename basename)			\
	{								\
		assert(nr);						\
		assert(names);						\
		watchman_expression_t* result =				\
			alloc_expr(WATCHMAN_EXPR_TY_##tyupper);		\
		result->e.name_expr.nr = nr;				\
		result->e.name_expr.names = malloc(nr * sizeof(char*));	\
		int i;							\
		for (i = 0; i < nr; ++i) {				\
			result->e.name_expr.names[i] = strdup(names[i]); \
		}							\
		return result;						\
	}
	
NAMES_EXPR(NAME, name)
NAMES_EXPR(INAME, iname)
#undef NAMES_EXPR

watchman_expression_t *watchman_type_expression(char c) {
	watchman_expression_t *result = alloc_expr(WATCHMAN_EXPR_TY_TYPE);
	result->e.type_expr.type = c;
	return result;
}