#include <ctype.h>

#include <haproxy/api.h>
#include <haproxy/arg.h>
#include <haproxy/buf.h>
#include <haproxy/cfgparse.h>
#include <haproxy/check.h>
#include <haproxy/cli.h>
#include <haproxy/global.h>
#include <haproxy/http.h>
#include <haproxy/http_rules.h>
#include <haproxy/list.h>
#include <haproxy/log.h>
#include <haproxy/sample.h>
#include <haproxy/session.h>
#include <haproxy/stream-t.h>
#include <haproxy/tcp_rules.h>
#include <haproxy/tcpcheck.h>
#include <haproxy/tools.h>
#include <haproxy/vars.h>


/* This contains a pool of struct vars */
DECLARE_STATIC_POOL(var_pool, "vars", sizeof(struct var));

/* list of variables for the process scope. */
struct vars proc_vars THREAD_ALIGNED(64);

/* This array contain all the names of all the HAProxy vars.
 * This permits to identify two variables name with
 * only one pointer. It permits to not using  strdup() for
 * each variable name used during the runtime.
 */
static char **var_names = NULL;
static int var_names_nb = 0;

/* This array of int contains the system limits per context. */
static unsigned int var_global_limit = 0;
static unsigned int var_global_size = 0;
static unsigned int var_proc_limit = 0;
static unsigned int var_sess_limit = 0;
static unsigned int var_txn_limit = 0;
static unsigned int var_reqres_limit = 0;
static unsigned int var_check_limit = 0;
static uint64_t var_name_hash_seed = 0;

__decl_rwlock(var_names_rwlock);

/* returns the struct vars pointer for a session, stream and scope, or NULL if
 * it does not exist.
 */
static inline struct vars *get_vars(struct session *sess, struct stream *strm, enum vars_scope scope)
{
	switch (scope) {
	case SCOPE_PROC:
		return &proc_vars;
	case SCOPE_SESS:
		return sess ? &sess->vars : NULL;
	case SCOPE_CHECK: {
			struct check *check = sess ? objt_check(sess->origin) : NULL;

			return check ? &check->vars : NULL;
		}
	case SCOPE_TXN:
		return strm ? &strm->vars_txn : NULL;
	case SCOPE_REQ:
	case SCOPE_RES:
	default:
		return strm ? &strm->vars_reqres : NULL;
	}
}

/* This function adds or remove memory size from the accounting. The inner
 * pointers may be null when setting the outer ones only.
 */
void var_accounting_diff(struct vars *vars, struct session *sess, struct stream *strm, int size)
{
	switch (vars->scope) {
	case SCOPE_REQ:
	case SCOPE_RES:
		if (strm)
			_HA_ATOMIC_ADD(&strm->vars_reqres.size, size);
		/* fall through */
	case SCOPE_TXN:
		if (strm)
			_HA_ATOMIC_ADD(&strm->vars_txn.size, size);
		goto scope_sess;
	case SCOPE_CHECK: {
			struct check *check = objt_check(sess->origin);

			if (check)
				_HA_ATOMIC_ADD(&check->vars.size, size);
		}
		/* fall through */
scope_sess:
	case SCOPE_SESS:
		_HA_ATOMIC_ADD(&sess->vars.size, size);
		/* fall through */
	case SCOPE_PROC:
		_HA_ATOMIC_ADD(&proc_vars.size, size);
		_HA_ATOMIC_ADD(&var_global_size, size);
	}
}

/* This function returns 1 if the <size> is available in the var
 * pool <vars>, otherwise returns 0. If the space is available,
 * the size is reserved. The inner pointers may be null when setting
 * the outer ones only. The accounting uses either <sess> or <strm>
 * depending on the scope. <strm> may be NULL when no stream is known
 * and only the session exists (eg: tcp-request connection).
 */
static int var_accounting_add(struct vars *vars, struct session *sess, struct stream *strm, int size)
{
	switch (vars->scope) {
	case SCOPE_REQ:
	case SCOPE_RES:
		if (var_reqres_limit && strm && strm->vars_reqres.size + size > var_reqres_limit)
			return 0;
		/* fall through */
	case SCOPE_TXN:
		if (var_txn_limit && strm && strm->vars_txn.size + size > var_txn_limit)
			return 0;
		goto scope_sess;
	case SCOPE_CHECK: {
			struct check *check = objt_check(sess->origin);

			if (var_check_limit && check && check->vars.size + size > var_check_limit)
				return 0;
		}
		/* fall through */
scope_sess:
	case SCOPE_SESS:
		if (var_sess_limit && sess->vars.size + size > var_sess_limit)
			return 0;
		/* fall through */
	case SCOPE_PROC:
		if (var_proc_limit && proc_vars.size + size > var_proc_limit)
			return 0;
		if (var_global_limit && var_global_size + size > var_global_limit)
			return 0;
	}
	var_accounting_diff(vars, sess, strm, size);
	return 1;
}

/* This function removes a variable from the list and frees the memory it was
 * using. If the variable is marked "VF_PERMANENT", the sample_data is only
 * reset to SMP_T_ANY unless <force> is non nul. Returns the freed size.
 */
unsigned int var_clear(struct var *var, int force)
{
	unsigned int size = 0;

	if (var->data.type == SMP_T_STR || var->data.type == SMP_T_BIN) {
		ha_free(&var->data.u.str.area);
		size += var->data.u.str.data;
	}
	else if (var->data.type == SMP_T_METH && var->data.u.meth.meth == HTTP_METH_OTHER) {
		ha_free(&var->data.u.meth.str.area);
		size += var->data.u.meth.str.data;
	}
	/* wipe the sample */
	var->data.type = SMP_T_ANY;

	if (!(var->flags & VF_PERMANENT) || force) {
		LIST_DELETE(&var->l);
		pool_free(var_pool, var);
		size += sizeof(struct var);
	}
	return size;
}

/* This function free all the memory used by all the variables
 * in the list.
 */
void vars_prune(struct vars *vars, struct session *sess, struct stream *strm)
{
	struct var *var, *tmp;
	unsigned int size = 0;

	HA_RWLOCK_WRLOCK(VARS_LOCK, &vars->rwlock);
	list_for_each_entry_safe(var, tmp, &vars->head, l) {
		size += var_clear(var, 1);
	}
	HA_RWLOCK_WRUNLOCK(VARS_LOCK, &vars->rwlock);
	var_accounting_diff(vars, sess, strm, -size);
}

/* This function frees all the memory used by all the session variables in the
 * list starting at <vars>.
 */
void vars_prune_per_sess(struct vars *vars)
{
	struct var *var, *tmp;
	unsigned int size = 0;

	HA_RWLOCK_WRLOCK(VARS_LOCK, &vars->rwlock);
	list_for_each_entry_safe(var, tmp, &vars->head, l) {
		size += var_clear(var, 1);
	}
	HA_RWLOCK_WRUNLOCK(VARS_LOCK, &vars->rwlock);

	_HA_ATOMIC_SUB(&vars->size, size);
	_HA_ATOMIC_SUB(&proc_vars.size, size);
	_HA_ATOMIC_SUB(&var_global_size, size);
}

/* This function initializes a variables list head */
void vars_init_head(struct vars *vars, enum vars_scope scope)
{
	LIST_INIT(&vars->head);
	vars->scope = scope;
	vars->size = 0;
	HA_RWLOCK_INIT(&vars->rwlock);
}

/* This function declares a new variable name. It returns a pointer
 * on the string identifying the name. This function assures that
 * the same name exists only once.
 *
 * This function check if the variable name is acceptable.
 *
 * The function returns NULL if an error occurs, and <err> is filled.
 * In this case, the HAProxy must be stopped because the structs are
 * left inconsistent. Otherwise, it returns the pointer on the global
 * name.
 */
static char *register_name(const char *name, int len, enum vars_scope *scope,
			   int alloc, char **err)
{
	int i;
	char **var_names2;
	const char *tmp;
	char *res = NULL;

	/* Check length. */
	if (len == 0) {
		memprintf(err, "Empty variable name cannot be accepted");
		return res;
	}

	/* Check scope. */
	if (len > 5 && strncmp(name, "proc.", 5) == 0) {
		name += 5;
		len -= 5;
		*scope = SCOPE_PROC;
	}
	else if (len > 5 && strncmp(name, "sess.", 5) == 0) {
		name += 5;
		len -= 5;
		*scope = SCOPE_SESS;
	}
	else if (len > 4 && strncmp(name, "txn.", 4) == 0) {
		name += 4;
		len -= 4;
		*scope = SCOPE_TXN;
	}
	else if (len > 4 && strncmp(name, "req.", 4) == 0) {
		name += 4;
		len -= 4;
		*scope = SCOPE_REQ;
	}
	else if (len > 4 && strncmp(name, "res.", 4) == 0) {
		name += 4;
		len -= 4;
		*scope = SCOPE_RES;
	}
	else if (len > 6 && strncmp(name, "check.", 6) == 0) {
		name += 6;
		len -= 6;
		*scope = SCOPE_CHECK;
	}
	else {
		memprintf(err, "invalid variable name '%.*s'. A variable name must be start by its scope. "
		               "The scope can be 'proc', 'sess', 'txn', 'req', 'res' or 'check'", len, name);
		return res;
	}

	if (alloc)
		HA_RWLOCK_WRLOCK(VARS_LOCK, &var_names_rwlock);
	else
		HA_RWLOCK_RDLOCK(VARS_LOCK, &var_names_rwlock);


	/* Look for existing variable name. */
	for (i = 0; i < var_names_nb; i++)
		if (strncmp(var_names[i], name, len) == 0 && var_names[i][len] == '\0') {
			res = var_names[i];
			goto end;
		}

	if (!alloc) {
		res = NULL;
		goto end;
	}

	/* Store variable name. If realloc fails, var_names remains valid */
	var_names2 = realloc(var_names, (var_names_nb + 1) * sizeof(*var_names));
	if (!var_names2) {
		memprintf(err, "out of memory error");
		res = NULL;
		goto end;
	}
	var_names_nb++;
	var_names = var_names2;
	var_names[var_names_nb - 1] = malloc(len + 1);
	if (!var_names[var_names_nb - 1]) {
		memprintf(err, "out of memory error");
		res = NULL;
		goto end;
	}
	memcpy(var_names[var_names_nb - 1], name, len);
	var_names[var_names_nb - 1][len] = '\0';

	/* Check variable name syntax. */
	tmp = var_names[var_names_nb - 1];
	while (*tmp) {
		if (!isalnum((unsigned char)*tmp) && *tmp != '_' && *tmp != '.') {
			memprintf(err, "invalid syntax at char '%s'", tmp);
			res = NULL;
			goto end;
		}
		tmp++;
	}
	res = var_names[var_names_nb - 1];

  end:
	if (alloc)
		HA_RWLOCK_WRUNLOCK(VARS_LOCK, &var_names_rwlock);
	else
		HA_RWLOCK_RDUNLOCK(VARS_LOCK, &var_names_rwlock);

	return res;
}

/* This function returns an existing variable or returns NULL. */
static inline struct var *var_get(struct vars *vars, const char *name)
{
	struct var *var;

	list_for_each_entry(var, &vars->head, l)
		if (var->name == name)
			return var;
	return NULL;
}

/* Returns 0 if fails, else returns 1. */
static int smp_fetch_var(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	const struct var_desc *var_desc = &args[0].data.var;
	const struct buffer *def = NULL;

	if (args[1].type == ARGT_STR)
		def = &args[1].data.str;

	return vars_get_by_desc(var_desc, smp, def);
}

/* This function tries to create variable <name> in scope <scope> and store
 * sample <smp> as its value. The stream and session are extracted from <smp>,
 * and the stream may be NULL when scope is SCOPE_SESS. In case there wouldn't
 * be enough memory to store the sample while the variable was already created,
 * it would be changed to a bool (which is memory-less).
 *
 * Flags is a bitfield that may contain one of the following flags:
 *   - VF_UPDATEONLY: if the scope is SCOPE_PROC, the variable may only be
 *     updated but not created.
 *   - VF_CREATEONLY: do nothing if the variable already exists (success).
 *   - VF_PERMANENT: this flag will be passed to the variable upon creation
 *
 * It returns 0 on failure, non-zero on success.
 */
static int var_set(const char *name, enum vars_scope scope, struct sample *smp, uint flags)
{
	struct vars *vars;
	struct var *var;
	int ret = 0;

	vars = get_vars(smp->sess, smp->strm, scope);
	if (!vars || vars->scope != scope)
		return 0;

	HA_RWLOCK_WRLOCK(VARS_LOCK, &vars->rwlock);

	/* Look for existing variable name. */
	var = var_get(vars, name);

	if (var) {
		if (flags & VF_CREATEONLY) {
			ret = 1;
			goto unlock;
		}

		/* free its used memory. */
		if (var->data.type == SMP_T_STR ||
		    var->data.type == SMP_T_BIN) {
			ha_free(&var->data.u.str.area);
			var_accounting_diff(vars, smp->sess, smp->strm,
					    -var->data.u.str.data);
		}
		else if (var->data.type == SMP_T_METH && var->data.u.meth.meth == HTTP_METH_OTHER) {
			ha_free(&var->data.u.meth.str.area);
			var_accounting_diff(vars, smp->sess, smp->strm,
					    -var->data.u.meth.str.data);
		}
	} else {
		/* creation permitted for proc ? */
		if (flags & VF_UPDATEONLY && scope == SCOPE_PROC)
			goto unlock;

		/* Check memory available. */
		if (!var_accounting_add(vars, smp->sess, smp->strm, sizeof(struct var)))
			goto unlock;

		/* Create new entry. */
		var = pool_alloc(var_pool);
		if (!var)
			goto unlock;
		LIST_APPEND(&vars->head, &var->l);
		var->name = name;
		var->flags = flags & VF_PERMANENT;
	}

	/* Set type. */
	var->data.type = smp->data.type;

	/* Copy data. If the data needs memory, the function can fail. */
	switch (var->data.type) {
	case SMP_T_BOOL:
	case SMP_T_SINT:
		var->data.u.sint = smp->data.u.sint;
		break;
	case SMP_T_IPV4:
		var->data.u.ipv4 = smp->data.u.ipv4;
		break;
	case SMP_T_IPV6:
		var->data.u.ipv6 = smp->data.u.ipv6;
		break;
	case SMP_T_STR:
	case SMP_T_BIN:
		if (!var_accounting_add(vars, smp->sess, smp->strm, smp->data.u.str.data)) {
			var->data.type = SMP_T_BOOL; /* This type doesn't use additional memory. */
			goto unlock;
		}

		var->data.u.str.area = malloc(smp->data.u.str.data);
		if (!var->data.u.str.area) {
			var_accounting_diff(vars, smp->sess, smp->strm,
					    -smp->data.u.str.data);
			var->data.type = SMP_T_BOOL; /* This type doesn't use additional memory. */
			goto unlock;
		}
		var->data.u.str.data = smp->data.u.str.data;
		memcpy(var->data.u.str.area, smp->data.u.str.area,
		       var->data.u.str.data);
		break;
	case SMP_T_METH:
		var->data.u.meth.meth = smp->data.u.meth.meth;
		if (smp->data.u.meth.meth != HTTP_METH_OTHER)
			break;

		if (!var_accounting_add(vars, smp->sess, smp->strm, smp->data.u.meth.str.data)) {
			var->data.type = SMP_T_BOOL; /* This type doesn't use additional memory. */
			goto unlock;
		}

		var->data.u.meth.str.area = malloc(smp->data.u.meth.str.data);
		if (!var->data.u.meth.str.area) {
			var_accounting_diff(vars, smp->sess, smp->strm,
					    -smp->data.u.meth.str.data);
			var->data.type = SMP_T_BOOL; /* This type doesn't use additional memory. */
			goto unlock;
		}
		var->data.u.meth.str.data = smp->data.u.meth.str.data;
		var->data.u.meth.str.size = smp->data.u.meth.str.data;
		memcpy(var->data.u.meth.str.area, smp->data.u.meth.str.area,
		       var->data.u.meth.str.data);
		break;
	}

	/* OK, now done */
	ret = 1;
 unlock:
	HA_RWLOCK_WRUNLOCK(VARS_LOCK, &vars->rwlock);
	return ret;
}

/* This unsets variable <name> from scope <scope>, using the session and stream
 * found in <smp>. Note that stream may be null for SCOPE_SESS. Returns 0 if
 * the scope was not found otherwise 1.
 */
static int var_unset(const char *name, enum vars_scope scope, struct sample *smp)
{
	struct vars *vars;
	struct var  *var;
	unsigned int size = 0;

	vars = get_vars(smp->sess, smp->strm, scope);
	if (!vars || vars->scope != scope)
		return 0;

	/* Look for existing variable name. */
	HA_RWLOCK_WRLOCK(VARS_LOCK, &vars->rwlock);
	var = var_get(vars, name);
	if (var) {
		size = var_clear(var, 0);
		var_accounting_diff(vars, smp->sess, smp->strm, -size);
	}
	HA_RWLOCK_WRUNLOCK(VARS_LOCK, &vars->rwlock);
	return 1;
}

/* Returns 0 if fails, else returns 1. */
static int smp_conv_store(const struct arg *args, struct sample *smp, void *private)
{
	return var_set(args[0].data.var.name, args[0].data.var.scope, smp, 0);
}

/* Returns 0 if fails, else returns 1. */
static int smp_conv_clear(const struct arg *args, struct sample *smp, void *private)
{
	return var_unset(args[0].data.var.name, args[0].data.var.scope, smp);
}

/* This functions check an argument entry and fill it with a variable
 * type. The argumen must be a string. If the variable lookup fails,
 * the function returns 0 and fill <err>, otherwise it returns 1.
 */
int vars_check_arg(struct arg *arg, char **err)
{
	char *name;
	enum vars_scope scope;
	struct sample empty_smp = { };

	/* Check arg type. */
	if (arg->type != ARGT_STR) {
		memprintf(err, "unexpected argument type");
		return 0;
	}

	/* Register new variable name. */
	name = register_name(arg->data.str.area, arg->data.str.data, &scope,
			     1,
			     err);
	if (!name)
		return 0;

	if (scope == SCOPE_PROC && !var_set(name, scope, &empty_smp, VF_CREATEONLY|VF_PERMANENT))
		return 0;

	/* properly destroy the chunk */
	chunk_destroy(&arg->data.str);

	/* Use the global variable name pointer. */
	arg->type = ARGT_VAR;
	arg->data.var.name = name;
	arg->data.var.scope = scope;
	return 1;
}

/* This function store a sample in a variable if it was already defined.
 * Returns zero on failure and non-zero otherwise. The variable not being
 * defined is treated as a failure.
 */
int vars_set_by_name_ifexist(const char *name, size_t len, struct sample *smp)
{
	enum vars_scope scope;

	/* Resolve name and scope. */
	name = register_name(name, len, &scope, 0, NULL);
	if (!name)
		return 0;

	return var_set(name, scope, smp, VF_UPDATEONLY);
}


/* This function store a sample in a variable.
 * Returns zero on failure and non-zero otherwise.
 */
int vars_set_by_name(const char *name, size_t len, struct sample *smp)
{
	enum vars_scope scope;

	/* Resolve name and scope. */
	name = register_name(name, len, &scope, 1, NULL);
	if (!name)
		return 0;

	return var_set(name, scope, smp, 0);
}

/* This function unset a variable if it was already defined.
 * Returns zero on failure and non-zero otherwise.
 */
int vars_unset_by_name_ifexist(const char *name, size_t len, struct sample *smp)
{
	enum vars_scope scope;

	/* Resolve name and scope. */
	name = register_name(name, len, &scope, 0, NULL);
	if (!name)
		return 0;

	return var_unset(name, scope, smp);
}


/* This retrieves variable <name> from variables <vars>, and if found and not
 * empty, duplicates the result into sample <smp>. smp_dup() is used in order
 * to release the variables lock ASAP (so a pre-allocated chunk is obtained
 * via get_trash_shunk()). The variables' lock is used for reads.
 *
 * The function returns 0 if the variable was not found and no default
 * value was provided in <def>, otherwise 1 with the sample filled.
 * Default values are always returned as strings.
 */
static int var_to_smp(struct vars *vars, const char *name, struct sample *smp, const struct buffer *def)
{
	struct var *var;

	/* Get the variable entry. */
	HA_RWLOCK_RDLOCK(VARS_LOCK, &vars->rwlock);
	var = var_get(vars, name);
	if (!var || !var->data.type) {
		if (!def) {
			HA_RWLOCK_RDUNLOCK(VARS_LOCK, &vars->rwlock);
			return 0;
		}

		/* not found but we have a default value */
		smp->data.type = SMP_T_STR;
		smp->data.u.str = *def;
	}
	else
		smp->data = var->data;

	/* Copy sample. */
	smp_dup(smp);

	HA_RWLOCK_RDUNLOCK(VARS_LOCK, &vars->rwlock);
	return 1;
}

/* This function fills a sample with the variable content.
 *
 * Keep in mind that a sample content is duplicated by using smp_dup()
 * and it therefore uses a pre-allocated trash chunk as returned by
 * get_trash_chunk().
 *
 * If the variable is not valid in this scope, 0 is always returned.
 * If the variable is valid but not found, either the default value
 * <def> is returned if not NULL, or zero is returned.
 *
 * Returns 1 if the sample is filled, otherwise it returns 0.
 */
int vars_get_by_name(const char *name, size_t len, struct sample *smp, const struct buffer *def)
{
	struct vars *vars;
	enum vars_scope scope;

	/* Resolve name and scope. */
	name = register_name(name, len, &scope, 0, NULL);
	if (!name)
		return 0;

	/* Select "vars" pool according with the scope. */
	vars = get_vars(smp->sess, smp->strm, scope);
	if (!vars || vars->scope != scope)
		return 0;


	return var_to_smp(vars, name, smp, def);
}

/* This function fills a sample with the content of the variable described
 * by <var_desc>.
 *
 * Keep in mind that a sample content is duplicated by using smp_dup()
 * and it therefore uses a pre-allocated trash chunk as returned by
 * get_trash_chunk().
 *
 * If the variable is not valid in this scope, 0 is always returned.
 * If the variable is valid but not found, either the default value
 * <def> is returned if not NULL, or zero is returned.
 *
 * Returns 1 if the sample is filled, otherwise it returns 0.
 */
int vars_get_by_desc(const struct var_desc *var_desc, struct sample *smp, const struct buffer *def)
{
	struct vars *vars;

	/* Select "vars" pool according with the scope. */
	vars = get_vars(smp->sess, smp->strm, var_desc->scope);

	/* Check if the scope is available a this point of processing. */
	if (!vars || vars->scope != var_desc->scope)
		return 0;

	return var_to_smp(vars, var_desc->name, smp, def);
}

/* Always returns ACT_RET_CONT even if an error occurs. */
static enum act_return action_store(struct act_rule *rule, struct proxy *px,
                                    struct session *sess, struct stream *s, int flags)
{
	struct buffer *fmtstr = NULL;
	struct sample smp;
	int dir;

	switch (rule->from) {
	case ACT_F_TCP_REQ_SES: dir = SMP_OPT_DIR_REQ; break;
	case ACT_F_TCP_REQ_CNT: dir = SMP_OPT_DIR_REQ; break;
	case ACT_F_TCP_RES_CNT: dir = SMP_OPT_DIR_RES; break;
	case ACT_F_HTTP_REQ:    dir = SMP_OPT_DIR_REQ; break;
	case ACT_F_HTTP_RES:    dir = SMP_OPT_DIR_RES; break;
	case ACT_F_TCP_CHK:     dir = SMP_OPT_DIR_REQ; break;
	case ACT_F_CFG_PARSER:  dir = SMP_OPT_DIR_REQ;  break; /* not used anyway */
	case ACT_F_CLI_PARSER:  dir = SMP_OPT_DIR_REQ;  break; /* not used anyway */
	default:
		send_log(px, LOG_ERR, "Vars: internal error while execute action store.");
		if (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE))
			ha_alert("Vars: internal error while execute action store.\n");
		return ACT_RET_CONT;
	}

	/* Process the expression. */
	memset(&smp, 0, sizeof(smp));

	if (!LIST_ISEMPTY(&rule->arg.vars.fmt)) {
		/* a format-string is used */

		fmtstr = alloc_trash_chunk();
		if (!fmtstr) {
			send_log(px, LOG_ERR, "Vars: memory allocation failure while processing store rule.");
			if (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE))
				ha_alert("Vars: memory allocation failure while processing store rule.\n");
			return ACT_RET_CONT;
		}

		/* execute the log-format expression */
		fmtstr->data = sess_build_logline(sess, s, fmtstr->area, fmtstr->size, &rule->arg.vars.fmt);

		/* convert it to a sample of type string as it's what the vars
		 * API consumes, and store it.
		 */
		smp_set_owner(&smp, px, sess, s, 0);
		smp.data.type = SMP_T_STR;
		smp.data.u.str = *fmtstr;
		var_set(rule->arg.vars.name, rule->arg.vars.scope, &smp, 0);
	}
	else {
		/* an expression is used */
		if (!sample_process(px, sess, s, dir|SMP_OPT_FINAL,
	                            rule->arg.vars.expr, &smp))
			return ACT_RET_CONT;
	}

	/* Store the sample, and ignore errors. */
	var_set(rule->arg.vars.name, rule->arg.vars.scope, &smp, 0);
	free_trash_chunk(fmtstr);
	return ACT_RET_CONT;
}

/* Always returns ACT_RET_CONT even if an error occurs. */
static enum act_return action_clear(struct act_rule *rule, struct proxy *px,
                                    struct session *sess, struct stream *s, int flags)
{
	struct sample smp;

	memset(&smp, 0, sizeof(smp));
	smp_set_owner(&smp, px, sess, s, SMP_OPT_FINAL);

	/* Clear the variable using the sample context, and ignore errors. */
	var_unset(rule->arg.vars.name, rule->arg.vars.scope, &smp);
	return ACT_RET_CONT;
}

static void release_store_rule(struct act_rule *rule)
{
	struct logformat_node *lf, *lfb;

	list_for_each_entry_safe(lf, lfb, &rule->arg.vars.fmt, list) {
		LIST_DELETE(&lf->list);
		release_sample_expr(lf->expr);
		free(lf->arg);
		free(lf);
	}

	release_sample_expr(rule->arg.vars.expr);
}

/* This two function checks the variable name and replace the
 * configuration string name by the global string name. its
 * the same string, but the global pointer can be easy to
 * compare. They return non-zero on success, zero on failure.
 *
 * The first function checks a sample-fetch and the second
 * checks a converter.
 */
static int smp_check_var(struct arg *args, char **err)
{
	return vars_check_arg(&args[0], err);
}

static int conv_check_var(struct arg *args, struct sample_conv *conv,
                          const char *file, int line, char **err_msg)
{
	return vars_check_arg(&args[0], err_msg);
}

/* This function is a common parser for using variables. It understands
 * the format:
 *
 *   set-var-fmt(<variable-name>) <format-string>
 *   set-var(<variable-name>) <expression>
 *   unset-var(<variable-name>)
 *
 * It returns ACT_RET_PRS_ERR if fails and <err> is filled with an error
 * message. Otherwise, it returns ACT_RET_PRS_OK and the variable <expr>
 * is filled with the pointer to the expression to execute. The proxy is
 * only used to retrieve the ->conf entries.
 */
static enum act_parse_ret parse_store(const char **args, int *arg, struct proxy *px,
                                      struct act_rule *rule, char **err)
{
	const char *var_name = args[*arg-1];
	int var_len;
	const char *kw_name;
	int flags, set_var = 0; /* 0=unset-var, 1=set-var, 2=set-var-fmt */
	struct sample empty_smp = { };

	if (strncmp(var_name, "set-var-fmt", 11) == 0) {
		var_name += 11;
		set_var   = 2;
	}
	else if (strncmp(var_name, "set-var", 7) == 0) {
		var_name += 7;
		set_var   = 1;
	}
	else if (strncmp(var_name, "unset-var", 9) == 0) {
		var_name += 9;
		set_var   = 0;
	}

	if (*var_name != '(') {
		memprintf(err, "invalid or incomplete action '%s'. Expects 'set-var(<var-name>)', 'set-var-fmt(<var-name>)' or 'unset-var(<var-name>)'",
			  args[*arg-1]);
		return ACT_RET_PRS_ERR;
	}
	var_name++; /* jump the '(' */
	var_len = strlen(var_name);
	var_len--; /* remove the ')' */
	if (var_name[var_len] != ')') {
		memprintf(err, "incomplete argument after action '%s'. Expects 'set-var(<var-name>)', 'set-var-fmt(<var-name>)' or 'unset-var(<var-name>)'",
			  args[*arg-1]);
		return ACT_RET_PRS_ERR;
	}

	LIST_INIT(&rule->arg.vars.fmt);
	rule->arg.vars.name = register_name(var_name, var_len, &rule->arg.vars.scope, 1, err);
	if (!rule->arg.vars.name)
		return ACT_RET_PRS_ERR;

	if (rule->arg.vars.scope == SCOPE_PROC &&
	    !var_set(rule->arg.vars.name, rule->arg.vars.scope, &empty_smp, VF_CREATEONLY|VF_PERMANENT))
		return 0;

	/* There is no fetch method when variable is unset. Just set the right
	 * action and return. */
	if (!set_var) {
		rule->action     = ACT_CUSTOM;
		rule->action_ptr = action_clear;
		rule->release_ptr = release_store_rule;
		return ACT_RET_PRS_OK;
	}

	kw_name = args[*arg-1];

	switch (rule->from) {
	case ACT_F_TCP_REQ_SES:
		flags = SMP_VAL_FE_SES_ACC;
		px->conf.args.ctx = ARGC_TSE;
		break;
	case ACT_F_TCP_REQ_CNT:
		flags = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_REQ_CNT : SMP_VAL_BE_REQ_CNT;
		px->conf.args.ctx = ARGC_TRQ;
		break;
	case ACT_F_TCP_RES_CNT:
		flags = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_RES_CNT : SMP_VAL_BE_RES_CNT;
		px->conf.args.ctx = ARGC_TRS;
		break;
	case ACT_F_HTTP_REQ:
		flags = (px->cap & PR_CAP_FE) ? SMP_VAL_FE_HRQ_HDR : SMP_VAL_BE_HRQ_HDR;
		px->conf.args.ctx = ARGC_HRQ;
		break;
	case ACT_F_HTTP_RES:
		flags = (px->cap & PR_CAP_BE) ? SMP_VAL_BE_HRS_HDR : SMP_VAL_FE_HRS_HDR;
		px->conf.args.ctx =  ARGC_HRS;
		break;
	case ACT_F_TCP_CHK:
		flags = SMP_VAL_BE_CHK_RUL;
		px->conf.args.ctx = ARGC_TCK;
		break;
	case ACT_F_CFG_PARSER:
		flags = SMP_VAL_CFG_PARSER;
		px->conf.args.ctx = ARGC_CFG;
		break;
	case ACT_F_CLI_PARSER:
		flags = SMP_VAL_CLI_PARSER;
		px->conf.args.ctx = ARGC_CLI;
		break;
	default:
		memprintf(err,
			  "internal error, unexpected rule->from=%d, please report this bug!",
			  rule->from);
		return ACT_RET_PRS_ERR;
	}

	if (set_var == 2) { /* set-var-fmt */
		if (!parse_logformat_string(args[*arg], px, &rule->arg.vars.fmt, 0, flags, err))
			return ACT_RET_PRS_ERR;

		(*arg)++;

		/* for late error reporting */
		free(px->conf.lfs_file);
		px->conf.lfs_file = strdup(px->conf.args.file);
		px->conf.lfs_line = px->conf.args.line;
	} else {
		/* set-var */
		rule->arg.vars.expr = sample_parse_expr((char **)args, arg, px->conf.args.file,
	                                                px->conf.args.line, err, &px->conf.args, NULL);
		if (!rule->arg.vars.expr)
			return ACT_RET_PRS_ERR;

		if (!(rule->arg.vars.expr->fetch->val & flags)) {
			memprintf(err,
			          "fetch method '%s' extracts information from '%s', none of which is available here",
			          kw_name, sample_src_names(rule->arg.vars.expr->fetch->use));
			free(rule->arg.vars.expr);
			return ACT_RET_PRS_ERR;
		}
	}

	rule->action     = ACT_CUSTOM;
	rule->action_ptr = action_store;
	rule->release_ptr = release_store_rule;
	return ACT_RET_PRS_OK;
}


/* parses a global "set-var" directive. It will create a temporary rule and
 * expression that are parsed, processed, and released on the fly so that we
 * respect the real set-var syntax. These directives take the following format:
 *    set-var <name> <expression>
 *    set-var-fmt <name> <fmt>
 * Note that parse_store() expects "set-var(name) <expression>" so we have to
 * temporarily replace the keyword here.
 */
static int vars_parse_global_set_var(char **args, int section_type, struct proxy *curpx,
                                     const struct proxy *defpx, const char *file, int line,
                                     char **err)
{
	struct proxy px = {
		.id = "CFG",
		.conf.args.file = file,
		.conf.args.line = line,
	};
	struct act_rule rule = {
		.arg.vars.scope = SCOPE_PROC,
		.from = ACT_F_CFG_PARSER,
	};
	enum obj_type objt = OBJ_TYPE_NONE;
	struct session *sess = NULL;
	enum act_parse_ret p_ret;
	char *old_arg1;
	char *tmp_arg1;
	int arg = 2; // variable name
	int ret = -1;
	int use_fmt = 0;

	LIST_INIT(&px.conf.args.list);

	use_fmt = strcmp(args[0], "set-var-fmt") == 0;

	if (!*args[1] || !*args[2]) {
		if (use_fmt)
			memprintf(err, "'%s' requires a process-wide variable name ('proc.<name>') and a format string.", args[0]);
		else
			memprintf(err, "'%s' requires a process-wide variable name ('proc.<name>') and a sample expression.", args[0]);
		goto end;
	}

	tmp_arg1 = NULL;
	if (!memprintf(&tmp_arg1, "set-var%s(%s)", use_fmt ? "-fmt" : "", args[1]))
		goto end;

	/* parse_store() will always return a message in <err> on error */
	old_arg1 = args[1]; args[1] = tmp_arg1;
	p_ret = parse_store((const char **)args, &arg, &px, &rule, err);
	free(args[1]); args[1] = old_arg1;

	if (p_ret != ACT_RET_PRS_OK)
		goto end;

	if (rule.arg.vars.scope != SCOPE_PROC) {
		memprintf(err, "'%s': cannot set variable '%s', only scope 'proc' is permitted in the global section.", args[0], args[1]);
		goto end;
	}

	if (smp_resolve_args(&px, err) != 0) {
		release_sample_expr(rule.arg.vars.expr);
		indent_msg(err, 2);
		goto end;
	}

	if (use_fmt && !(sess = session_new(&px, NULL, &objt))) {
		release_sample_expr(rule.arg.vars.expr);
		memprintf(err, "'%s': out of memory when trying to set variable '%s' in the global section.", args[0], args[1]);
		goto end;
	}

	action_store(&rule, &px, sess, NULL, 0);
	release_sample_expr(rule.arg.vars.expr);
	if (sess)
		session_free(sess);

	ret = 0;
 end:
	return ret;
}

/* parse CLI's "get var <name>" */
static int vars_parse_cli_get_var(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct vars *vars;
	struct sample smp = { };
	int i;

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	if (!*args[2])
		return cli_err(appctx, "Missing process-wide variable identifier.\n");

	vars = get_vars(NULL, NULL, SCOPE_PROC);
	if (!vars || vars->scope != SCOPE_PROC)
		return 0;

	if (!vars_get_by_name(args[2], strlen(args[2]), &smp, NULL))
		return cli_err(appctx, "Variable not found.\n");

	/* the sample returned by vars_get_by_name() is allocated into a trash
	 * chunk so we have no constraint to manipulate it.
	 */
	chunk_printf(&trash, "%s: type=%s value=", args[2], smp_to_type[smp.data.type]);

	if (!sample_casts[smp.data.type][SMP_T_STR] ||
	    !sample_casts[smp.data.type][SMP_T_STR](&smp)) {
		chunk_appendf(&trash, "(undisplayable)");
	} else {
		/* Display the displayable chars*. */
		b_putchr(&trash, '<');
		for (i = 0; i < smp.data.u.str.data; i++) {
			if (isprint((unsigned char)smp.data.u.str.area[i]))
				b_putchr(&trash, smp.data.u.str.area[i]);
			else
				b_putchr(&trash, '.');
		}
		b_putchr(&trash, '>');
		b_putchr(&trash, 0);
	}
	return cli_msg(appctx, LOG_INFO, trash.area);
}

/* parse CLI's "set var <name>". It accepts:
 *  - set var <name> <expression>
 *  - set var <name> expr <expression>
 *  - set var <name> fmt <format>
 */
static int vars_parse_cli_set_var(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct proxy px = {
		.id = "CLI",
		.conf.args.file = "CLI",
		.conf.args.line = 0,
	};
	struct act_rule rule = {
		.arg.vars.scope = SCOPE_PROC,
		.from = ACT_F_CLI_PARSER,
	};
	enum obj_type objt = OBJ_TYPE_NONE;
	struct session *sess = NULL;
	enum act_parse_ret p_ret;
	const char *tmp_args[3];
	int tmp_arg;
	char *tmp_act;
	char *err = NULL;
	int nberr;
	int use_fmt = 0;

	LIST_INIT(&px.conf.args.list);

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	if (!*args[2])
		return cli_err(appctx, "Missing process-wide variable identifier.\n");

	if (!*args[3])
		return cli_err(appctx, "Missing either 'expr', 'fmt' or expression.\n");

	if (*args[4]) {
		/* this is the long format */
		if (strcmp(args[3], "fmt") == 0)
			use_fmt = 1;
		else if (strcmp(args[3], "expr") != 0) {
			memprintf(&err, "'%s %s': arg type must be either 'expr' or 'fmt' but got '%s'.", args[0], args[1], args[3]);
			goto fail;
		}
	}

	tmp_act = NULL;
	if (!memprintf(&tmp_act, "set-var%s(%s)", use_fmt ? "-fmt" : "", args[2])) {
		memprintf(&err, "memory allocation error.");
		goto fail;
	}

	/* parse_store() will always return a message in <err> on error */
	tmp_args[0] = tmp_act;
	tmp_args[1] = (*args[4]) ? args[4] : args[3];
	tmp_args[2] = "";
	tmp_arg = 1; // must point to the first arg after the action
	p_ret = parse_store(tmp_args, &tmp_arg, &px, &rule, &err);
	free(tmp_act);

	if (p_ret != ACT_RET_PRS_OK)
		goto fail;

	if (rule.arg.vars.scope != SCOPE_PROC) {
		memprintf(&err, "'%s %s': cannot set variable '%s', only scope 'proc' is permitted here.", args[0], args[1], args[2]);
		goto fail;
	}

	err = NULL;
	nberr = smp_resolve_args(&px, &err);
	if (nberr) {
		release_sample_expr(rule.arg.vars.expr);
		indent_msg(&err, 2);
		goto fail;
	}

	if (use_fmt && !(sess = session_new(&px, NULL, &objt))) {
		release_sample_expr(rule.arg.vars.expr);
		memprintf(&err, "memory allocation error.");
		goto fail;
	}

	action_store(&rule, &px, sess, NULL, 0);
	release_sample_expr(rule.arg.vars.expr);
	if (sess)
		session_free(sess);

	appctx->st0 = CLI_ST_PROMPT;
	return 0;
 fail:
	return cli_dynerr(appctx, err);
}

static int vars_max_size(char **args, int section_type, struct proxy *curpx,
                         const struct proxy *defpx, const char *file, int line,
                         char **err, unsigned int *limit)
{
	char *error;

	*limit = strtol(args[1], &error, 10);
	if (*error != 0) {
		memprintf(err, "%s: '%s' is an invalid size", args[0], args[1]);
		return -1;
	}
	return 0;
}

static int vars_max_size_global(char **args, int section_type, struct proxy *curpx,
                                const struct proxy *defpx, const char *file, int line,
                                char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_global_limit);
}

static int vars_max_size_proc(char **args, int section_type, struct proxy *curpx,
                                const struct proxy *defpx, const char *file, int line,
                                char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_proc_limit);
}

static int vars_max_size_sess(char **args, int section_type, struct proxy *curpx,
                              const struct proxy *defpx, const char *file, int line,
                              char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_sess_limit);
}

static int vars_max_size_txn(char **args, int section_type, struct proxy *curpx,
                             const struct proxy *defpx, const char *file, int line,
                             char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_txn_limit);
}

static int vars_max_size_reqres(char **args, int section_type, struct proxy *curpx,
                                const struct proxy *defpx, const char *file, int line,
                                char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_reqres_limit);
}

static int vars_max_size_check(char **args, int section_type, struct proxy *curpx,
                                const struct proxy *defpx, const char *file, int line,
                                char **err)
{
	return vars_max_size(args, section_type, curpx, defpx, file, line, err, &var_check_limit);
}

/* early boot initialization */
static void vars_init()
{
	var_name_hash_seed = ha_random64();
}

INITCALL0(STG_PREPARE, vars_init);

static void vars_deinit()
{
	while (var_names_nb-- > 0)
		free(var_names[var_names_nb]);
	free(var_names);
}

REGISTER_POST_DEINIT(vars_deinit);

static struct sample_fetch_kw_list sample_fetch_keywords = {ILH, {

	{ "var", smp_fetch_var, ARG2(1,STR,STR), smp_check_var, SMP_T_STR, SMP_USE_CONST },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &sample_fetch_keywords);

static struct sample_conv_kw_list sample_conv_kws = {ILH, {
	{ "set-var",   smp_conv_store, ARG1(1,STR), conv_check_var, SMP_T_ANY, SMP_T_ANY },
	{ "unset-var", smp_conv_clear, ARG1(1,STR), conv_check_var, SMP_T_ANY, SMP_T_ANY },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_convs, &sample_conv_kws);

static struct action_kw_list tcp_req_sess_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_req_sess_keywords_register, &tcp_req_sess_kws);

static struct action_kw_list tcp_req_cont_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_req_cont_keywords_register, &tcp_req_cont_kws);

static struct action_kw_list tcp_res_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_res_cont_keywords_register, &tcp_res_kws);

static struct action_kw_list tcp_check_kws = {ILH, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_check_keywords_register, &tcp_check_kws);

static struct action_kw_list http_req_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, http_req_keywords_register, &http_req_kws);

static struct action_kw_list http_res_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, http_res_keywords_register, &http_res_kws);

static struct action_kw_list http_after_res_kws = { { }, {
	{ "set-var-fmt", parse_store, KWF_MATCH_PREFIX },
	{ "set-var",   parse_store, KWF_MATCH_PREFIX },
	{ "unset-var", parse_store, KWF_MATCH_PREFIX },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, http_after_res_keywords_register, &http_after_res_kws);

static struct cfg_kw_list cfg_kws = {{ },{
	{ CFG_GLOBAL, "set-var",              vars_parse_global_set_var },
	{ CFG_GLOBAL, "set-var-fmt",          vars_parse_global_set_var },
	{ CFG_GLOBAL, "tune.vars.global-max-size", vars_max_size_global },
	{ CFG_GLOBAL, "tune.vars.proc-max-size",   vars_max_size_proc   },
	{ CFG_GLOBAL, "tune.vars.sess-max-size",   vars_max_size_sess   },
	{ CFG_GLOBAL, "tune.vars.txn-max-size",    vars_max_size_txn    },
	{ CFG_GLOBAL, "tune.vars.reqres-max-size", vars_max_size_reqres },
	{ CFG_GLOBAL, "tune.vars.check-max-size",  vars_max_size_check  },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);


/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "get",   "var", NULL }, "get var <name>                          : retrieve contents of a process-wide variable", vars_parse_cli_get_var, NULL },
	{ { "set",   "var", NULL }, "set var <name> [fmt|expr] {<fmt>|<expr>}: set variable from an expression or a format",  vars_parse_cli_set_var, NULL, NULL, NULL, ACCESS_EXPERIMENTAL },
	{ { NULL }, NULL, NULL, NULL }
}};
INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);
