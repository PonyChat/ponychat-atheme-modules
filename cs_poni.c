#include "atheme-compat.h"

DECLARE_MODULE_V1
(
	"contrib/cs_poni", false, _modinit, _moddeinit,
	PACKAGE_STRING,
	"PonyChat Network <https://ponychat.net>"
);

static void cs_cmd_poni(sourceinfo_t *si, int parc, char *parv[]);

command_t cs_poni = { "PONI", "Verifies network ponies by responding with poni.",
			AC_NONE, 0, cs_cmd_poni, { .path = "contrib/cs_poni" } };

void _modinit(module_t *m)
{
	service_named_bind_command("chanserv", &cs_poni);
}

void _moddeinit(module_unload_intent_t intent)
{
	service_named_unbind_command("chanserv", &cs_poni);
}

static void cs_cmd_poni(sourceinfo_t *si, int parc, char *parv[])
{
	command_success_nodata(si, "poni");
	return;
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
