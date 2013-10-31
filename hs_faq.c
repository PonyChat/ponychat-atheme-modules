#include "atheme.h"

DECLARE_MODULE_V1
(
	"contrib/hs_faq", true, _modinit, _moddeinit,
	PACKAGE_STRING,
	"PonyChat Network <http://ponychat.net/>"
);

static void hs_cmd_faq(sourceinfo_t *si, int parc, char *parv[]);

static void write_faqdb(database_handle_t *db);
static void db_h_faq(database_handle_t *db, const char *type);

command_t hs_faq = { "FAQ", N_("Manage or view frequently asked questions."), PRIV_USER_ADMIN, 3, hs_cmd_faq, { .path = "contrib/hs_faq" } };

struct faq_ {
	char *name;
	char *contents;
};

typedef struct faq_ faq_t;

mowgli_list_t hs_faqlist;
mowgli_random_t *r;

void _modinit(module_t *m)
{
	if (!module_find_published("backend/opensex"))
	{
		slog(LG_INFO, "Module %s requires use of the OpenSEX database backend, refusing to load.", m->name);
		m->mflags = MODTYPE_FAIL;
		return;
	}

	hook_add_db_write(write_faqdb);

	db_register_type_handler("FAQ", db_h_faq);

	service_named_bind_command("helpserv", &hs_faq);
	service_named_bind_command("chanserv", &hs_faq);
}

void _moddeinit(module_unload_intent_t intent)
{
	hook_del_db_write(write_faqdb);

	db_unregister_type_handler("FAQ");

	service_named_unbind_command("helpserv", &hs_faq);
	service_named_unbind_command("chanserv", &hs_faq);
}

static void write_faqdb(database_handle_t *db)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, hs_faqlist.head)
	{
		faq_t *l = n->data;

		db_start_row(db, "FAQ");
		db_write_word(db, l->name);
		db_write_str(db, l->contents);
		db_commit_row(db);
	}
}

static void db_h_faq(database_handle_t *db, const char *type)
{
	const char *name = db_sread_word(db);
	const char *contents = db_sread_str(db);

	faq_t *faq = smalloc(sizeof(faq_t));
	faq->name = sstrdup(name);
	faq->contents = sstrdup(contents);
	mowgli_node_add(faq, mowgli_node_create(), &hs_faqlist);
}

static void hs_cmd_faq(sourceinfo_t *si, int parc, char *parv[])
{
	mowgli_node_t *n, *tn;
	faq_t *faq;

	service_t *svs = service_find("helpserv");

	unsigned int in_channel = 0;

	if (si->c != NULL)
        {
		++in_channel;
	}

	char *action 	= parv[in_channel];
	char *name 	= parv[in_channel + 1];
	char *contents 	= parv[in_channel + 2];

	if (si->smu == NULL)
	{
		command_fail(si, fault_noprivs, _("You are not logged in."));
		return;
	}

	if (!action)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "FAQ");
		command_fail(si, fault_needmoreparams, _("Syntax: FAQ <name> | <action> <parameters>"));
		return;
	}

	if (!strcasecmp("ADD", action))
	{
		if (in_channel)
		{
			/* XXX */
			command_success_nodata(si, _("This command cannot be used in channels."));
			return;
		}

		if (!contents)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "FAQ");
			command_fail(si, fault_needmoreparams, _("Syntax: FAQ ADD <name> <contents>"));
			return;
		}

		MOWGLI_ITER_FOREACH(n, hs_faqlist.head)
		{
			faq = n->data;

			if (!strcasecmp(faq->name, name))
			{
				command_success_nodata(si, _("\2%s\2 is already in the FAQ database."), name);
				return;
			}
		}

		faq = smalloc(sizeof(faq_t));
		faq->name = sstrdup(name);
		faq->contents = sstrdup(contents);

		logcommand(si, CMDLOG_ADMIN, "FAQ:ADD: \2%s\2 %s", name, contents);

		n = mowgli_node_create();
		mowgli_node_add(faq, n, &hs_faqlist);

		command_success_nodata(si, _("FAQ \2%s\2 has been added to the database."), name);
		return;
	}
	else if (!strcasecmp("DEL", action))
	{
		if (!name)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "FAQ");
			command_fail(si, fault_needmoreparams, _("Syntax: FAQ DEL <name>"));
			return;
		}

		MOWGLI_ITER_FOREACH_SAFE(n, tn, hs_faqlist.head)
		{
			faq = n->data;

			if(!strcasecmp(faq->name, name))
			{
				logcommand(si, CMDLOG_ADMIN, "FAQ:DEL: \2%s\2", faq->name);

				mowgli_node_delete(n, &hs_faqlist);

				free(faq->name);
				free(faq->contents);
				free(faq);

				command_success_nodata(si, _("FAQ \2%s\2 has been deleted from the database."), name);

				return;
			}
		}

		command_success_nodata(si, _("FAQ \2%s\2 was not found in the database."), name);

		return;

	}
	else if (!strcasecmp("LIST", action))
	{
		MOWGLI_ITER_FOREACH(n, hs_faqlist.head)
		{
			faq = n->data;

			command_success_nodata(si, "\2%s:\2 %s",
				faq->name, faq->contents);
		}
		command_success_nodata(si, "End of list.");
		logcommand(si, CMDLOG_GET, "FAQ:LIST");
		return;
	}
	else
	{

		MOWGLI_ITER_FOREACH(n, hs_faqlist.head)
		{
			faq = n->data;

			if (!strcasecmp(faq->name, action))
			{
				if (in_channel)
					msg(svs->me->nick, si->c->name,
							"\2%s\2: %s",
							faq->name, faq->contents);
				else
					command_success_nodata(si, "\2%s\2: %s",
							faq->name, faq->contents);

				return;
			}
		}

		if (in_channel)
			msg(svs->me->nick, si->c->name, "No such FAQ.");
		else
			command_success_nodata(si, "No such FAQ.");

		return;
	}
}



/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */

