/*
 * Copyright (c) 2013 Alex Iadicico <alex -at- ajitek.net>
 * Rights to this code are as documented in doc/LICENSE.
 */

#include "atheme-compat.h"

DECLARE_MODULE_V1
(
	"contrib/cs_flood", false, _modinit, _moddeinit,
	PACKAGE_STRING,
	"Atheme Development Group <http://www.atheme.net>"
);

#define PARAMS_META "flood"
#define PARAMS_PRIV "flood:params"
#define SCORES_PRIV "flood:scores"

#if 0
#define L(x...) slog(LG_DEBUG, "cs_flood: " x)
#else
#define L(x...)
#endif

#define FLOOD_ACTION_INVALID '?'
#define FLOOD_ACTION_KICK 'k'
#define FLOOD_ACTION_BAN 'b'
#define FLOOD_ACTION_KICKBAN 'K'
#define FLOOD_ACTION_QUIET 'q'

struct floodparams {
	char action;
	float penalty, max;
};

struct floodscore {
	struct timeval msg;
	float score;
};

static mowgli_heap_t *fp_heap;
static mowgli_heap_t *fs_heap;

static chanban_t *(*place_quietmask)(channel_t *c, int dir, const char *hostbuf) = NULL;

static void *privatedata_delete(void *target, const char *key)
{
	object_t *obj;

	obj = object(target);
	if (obj->privatedata == NULL)
		return NULL;

	return mowgli_patricia_delete(obj->privatedata, key);
}

static int fp_decode(struct floodparams *fp, char *md)
{
	fp->action = *md++;
	if (*md++ != ':') return -1;
	if (*md == '\0') return -1;
	fp->penalty = strtof(md, &md);
	if (*md++ != ':') return -1;
	if (*md == '\0') return -1;
	fp->max = strtof(md, &md);
	if (*md != '\0') return -1;
	L("%c penalty=%f max=%f", fp->action, fp->penalty, fp->max);
	return 0;
}

static const char *fp_encode(struct floodparams *fp)
{
	static char buf[256];
	snprintf(buf, 256, "%c:%.2f:%.2f", fp->action, fp->penalty, fp->max);
	return buf;
}

static struct floodparams *fp_load(mychan_t *mc)
{
	struct floodparams *fp;
	metadata_t *md;

	if ((fp = privatedata_get(mc, PARAMS_PRIV)) != NULL)
		return fp;

	if ((md = metadata_find(mc, PARAMS_META)) == NULL)
		return NULL;

	L("creating flood params for %s in fp_load", mc->name);

	fp = mowgli_heap_alloc(fp_heap);

	if (fp_decode(fp, md->value) < 0) {
		mowgli_log("'%s' is not valid flood control metadata!", md->value);
		/* TODO: send notice to chanops? */
		mowgli_heap_free(fp_heap, fp);
		metadata_delete(mc, PARAMS_META);
		privatedata_delete(mc, PARAMS_PRIV);
		return NULL;
	}

	privatedata_set(mc, PARAMS_PRIV, fp);

	return fp;
}

static void fp_set(mychan_t *mc, struct floodparams *nfp)
{
	struct floodparams *fp;

	fp = privatedata_get(mc, PARAMS_PRIV);

	if (fp == NULL) {
		L("creating flood params for %s in fp_set", mc->name);
		fp = mowgli_heap_alloc(fp_heap);
		privatedata_set(mc, PARAMS_PRIV, fp);
	}

	memcpy(fp, nfp, sizeof(*fp));

	metadata_delete(mc, PARAMS_META); /* sigh... */
	metadata_add(mc, PARAMS_META, fp_encode(nfp));

	L("set flood:params to %s", fp_encode(nfp));
}

static void fp_clear(mychan_t *mc, bool total)
{
	struct floodparams *fp;

	L("clearing flood params from %s", mc->name);

	if ((fp = privatedata_get(mc, PARAMS_PRIV)) != NULL) {
		mowgli_heap_free(fp_heap, fp);
		privatedata_delete(mc, PARAMS_PRIV);
	}

	if (total)
		metadata_delete(mc, PARAMS_META);
}

static struct floodscore *fs_new(struct floodparams *fp)
{
	struct floodscore *fs;

	if (fs_heap == NULL)
		return NULL;

	L("+++ SCORES");

	fs = mowgli_heap_alloc(fs_heap);

	gettimeofday(&fs->msg, NULL);
	fs->score = fp->penalty * 1.8;
	if (fs->score > fp->max)
		fs->score = fp->max;

	return fs;
}

static void fs_del(mychan_t *mc, user_t *u)
{
	mowgli_patricia_t *scores;
	struct floodscore *fs;

	if ((scores = privatedata_get(mc, SCORES_PRIV)) == NULL)
		return;
	if ((fs = mowgli_patricia_retrieve(scores, u->uid)) == NULL)
		return;

	L("--- SCORES");

	mowgli_heap_free(fs_heap, fs);
	mowgli_patricia_delete(scores, u->uid);
}

static struct floodscore *fs_load(mychan_t *mc, struct floodparams *fp, user_t *u)
{
	mowgli_patricia_t *scores;
	struct floodscore *fs;

	if ((scores = privatedata_get(mc, SCORES_PRIV)) == NULL) {
		L("creating score database for %s", mc->name);
		scores = mowgli_patricia_create(NULL);
		privatedata_set(mc, SCORES_PRIV, scores);
	}

	if ((fs = mowgli_patricia_retrieve(scores, u->uid)) == NULL) {
		L("creating score for %s", u->nick);
		fs = fs_new(fp);
		mowgli_patricia_add(scores, u->uid, fs);
	}

	return fs;
}

static void fs_clear_cb(const char *key, void *data, void *priv)
{
	L("--- SCORES");
	mowgli_heap_free(fs_heap, data);
}

static void fs_clear(mychan_t *mc, bool total)
{
	mowgli_patricia_t *scores;

	if ((scores = privatedata_get(mc, SCORES_PRIV)) == NULL)
		return;

	L("clearing scores database for %s", mc->name);

	mowgli_patricia_destroy(scores, fs_clear_cb, NULL);
	privatedata_delete(mc, SCORES_PRIV);
}

/* if total, all settings are deleted as well */
static void flood_clear(mychan_t *mc, bool total)
{
	fs_clear(mc, total);
	fp_clear(mc, total);
}

static void do_one_devoice(char mchar, user_t *u, channel_t *c)
{
	char buf[3];

	buf[0] = '-';
	buf[1] = mchar;
	buf[2] = '\0';

	channel_mode_va(chansvs.me->me, c, 2, buf, u->nick);
}

static void do_quiet(user_t *u, channel_t *c)
{
	char hostbuf[BUFSIZE];
	chanban_t *cb;
	chanuser_t *cu;

	/* duplicates a lot of work in chanserv/quiet's devoice_user, but
	   we cannot use devoice_user as it needs a sourceinfo_t */

	cu = chanuser_find(c, u);
	if (cu == NULL)
		return; /* wat */

	if (cu->modes & CSTATUS_OP)
		do_one_devoice('o', u, c);
	if (cu->modes & CSTATUS_VOICE)
		do_one_devoice('v', u, c);
	if (ircd->uses_owner && (cu->modes & ircd->owner_mode))
		do_one_devoice(ircd->owner_mchar[1], u, c);
	if (ircd->uses_protect && (cu->modes & ircd->protect_mode))
		do_one_devoice(ircd->protect_mchar[1], u, c);
	if (ircd->uses_halfops && (cu->modes & ircd->halfops_mode))
		do_one_devoice(ircd->halfops_mchar[1], u, c);

	if (!place_quietmask)
		return; /* XXX */

	mowgli_strlcpy(hostbuf, "*!*@", BUFSIZE);
	mowgli_strlcat(hostbuf, u->vhost, BUFSIZE);

	slog(LG_INFO, "FLOOD: QUIET \2%s\2 on \2%s\2",
	     hostbuf, c->name);

	cb = place_quietmask(c, MTYPE_ADD, hostbuf);
}

static void on_channel_message(hook_cmessage_data_t *data)
{
	mychan_t *mc = MYCHAN_FROM(data->c);
	struct floodparams *fp;
	struct floodscore *fs;
	struct timeval tv, now;
	float diff;

	if (mc == NULL)
		return;

	if ((fp = fp_load(mc)) == NULL)
		return;

	if ((fs = fs_load(mc, fp, data->u)) == NULL) {
		slog(LG_ERROR, "fs_load failed for %s", mc->name);
		return;
	}

	gettimeofday(&now, NULL);
	timersub(&now, &fs->msg, &tv);
	diff = tv.tv_sec + (tv.tv_usec * 1e-6) - fp->penalty;

	if (fs->score + diff > fp->max)
		fs->score = fp->max;
	else
		fs->score += diff;

	fs->msg.tv_sec = now.tv_sec;
	fs->msg.tv_usec = now.tv_usec;

	L("[%s:%s] diff=%.3f score=%.3f (max=%.3f)", mc->name, data->u->nick, diff, fs->score, fp->max);

	if (fs->score > 0)
		return;

	/* user is flooding */

	fs->score = 0;

	switch (fp->action) {
	case FLOOD_ACTION_KICKBAN:
		ban(chansvs.me->me, mc->chan, data->u);
	case FLOOD_ACTION_KICK:
		slog(LG_INFO, "FLOOD: KICK%s \2%s!%s@%s\2 on \2%s\2",
		     fp->action == FLOOD_ACTION_KICK ? "" : "BAN",
		     data->u->nick, data->u->user, data->u->vhost, data->c->name);
		try_kick(chansvs.me->me, mc->chan, data->u, "Flooding");
		break;

	case FLOOD_ACTION_QUIET:
		do_quiet(data->u, mc->chan);
		break;
	}
}

static void on_channel_part(hook_channel_joinpart_t *data)
{
	mychan_t *mc = MYCHAN_FROM(data->cu->chan);
	user_t *u = data->cu->user;
	mowgli_patricia_t *scores;
	struct floodscore *fs;

	if (mc == NULL)
		return;

	fs_del(mc, u);
}

static void on_channel_drop(mychan_t *mc)
{
	flood_clear(mc, true);
}

static char *slice(char **s)
{
	if (!*s)
		return NULL;
	*s = strchr(*s, ' ');
	if (!*s)
		return NULL;
	*(*s)++ = '\0';
	while (**s && isspace(**s))
		(*s)++;
	return *s;
}

static char get_action(char *act)
{
	if (!strcasecmp(act, "KICK"))
		return FLOOD_ACTION_KICK;
	if (!strcasecmp(act, "KICKBAN"))
		return FLOOD_ACTION_KICKBAN;
	if (!strcasecmp(act, "QUIET") && place_quietmask)
		return FLOOD_ACTION_QUIET;
	/*
	only because BAN makes *no sense*
	if (!strcasecmp(act, "BAN"))
		return FLOOD_ACTION_BAN;
	*/
	return FLOOD_ACTION_INVALID;
}

static void cs_set_cmd_flood(sourceinfo_t *si, int parc, char *parv[])
{
	mychan_t *mc;
	char *s;
	struct floodparams fp;

	if (!(mc = mychan_find(parv[0])))
	{
		command_fail(si, fault_nosuch_target, _("Channel \2%s\2 is not registered."), parv[0]);
		return;
	}

	if (!chanacs_source_has_flag(mc, si, CA_SET))
	{
		command_fail(si, fault_noprivs, _("You are not authorized to perform this command."));
		return;
	}

	s = parv[1];
	parv[2] = slice(&s);
	parv[3] = slice(&s);

	if (!parv[1]) {
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET FLOOD");
		return;
	}

	if (!strcasecmp(parv[1], "OFF")) {
		logcommand(si, CMDLOG_SET, "SET:FLOOD: \2%s OFF\2", mc->name);
		flood_clear(mc, true);
		command_success_nodata(si, "Flood control turned off in \2%s\2", mc->name);
		return;
	}

	if (!parv[2] || !parv[3]) {
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET FLOOD");
		return;
	}

	for (s=parv[1]; *s; s++)
		*s = toupper(*s);
		
	fp.action = get_action(parv[1]);
	fp.penalty = strtof(parv[2], NULL);
	fp.max = strtof(parv[3], NULL);

	if (fp.action == FLOOD_ACTION_INVALID) {
		command_fail(si, fault_badparams, "Unknown action \2%s\2", parv[1]);
		return;
	}

	if (fp.penalty < 1) {
		command_fail(si, fault_badparams, "Flood penalty must be at least 1");
		return;
	}

	if (fp.max < 1) {
		command_fail(si, fault_badparams, "Flood score maximum must be at least 1");
		return;
	}

	if (fp.penalty >= fp.max) {
		command_fail(si, fault_badparams, "Flood penalty must be less than the maximum score");
		return;
	}

	fp_set(mc, &fp);
	logcommand(si, CMDLOG_SET, "SET:FLOOD: \2%s %s\2", mc->name, fp_encode(&fp));
	command_success_nodata(si, "Set flood control in \2%s\2", mc->name);
}

command_t cs_set_flood = {
	"FLOOD", N_("Set flood control parameters"), AC_NONE, 2,
	cs_set_cmd_flood, { .path = "contrib/set_flood" }
};
mowgli_patricia_t **cs_set_cmdtree;

void _modinit(module_t *m)
{
	MODULE_TRY_REQUEST_SYMBOL(m, cs_set_cmdtree, "chanserv/set_core", "cs_set_cmdtree");

	if (module_request("chanserv/quiet"))
		place_quietmask = module_locate_symbol("chanserv/quiet", "place_quietmask");

	hook_add_event("channel_message");
	hook_add_channel_message(on_channel_message);

	hook_add_event("channel_part");
	hook_add_channel_part(on_channel_part);

	hook_add_event("channel_drop");
	hook_add_channel_drop(on_channel_drop);

	L("cs_flood loaded");

	fp_heap = mowgli_heap_create(sizeof(struct floodparams), 32, BH_DONTCARE);
	fs_heap = mowgli_heap_create(sizeof(struct floodscore), 256, BH_DONTCARE);

	command_add(&cs_set_flood, *cs_set_cmdtree);
}

void _moddeinit(module_unload_intent_t intent)
{
	mowgli_patricia_iteration_state_t iter;
	channel_t *c;
	mychan_t *mc;

	place_quietmask = NULL;

	hook_del_channel_message(on_channel_message);
	hook_del_channel_part(on_channel_part);

	/* move to another function? */
	MOWGLI_PATRICIA_FOREACH(c, &iter, chanlist) {
		mc = MYCHAN_FROM(c);
		if (mc != NULL)
			flood_clear(mc, false);
	}

	mowgli_heap_destroy(fp_heap);
	mowgli_heap_destroy(fs_heap);

	command_delete(&cs_set_flood, *cs_set_cmdtree);
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
