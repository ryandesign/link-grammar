/*
 * lookup-atomese.cc
 *
 * Implement the word-lookup callbacks
 *
 * Copyright (c) 2022 Linas Vepstas <linasvepstas@gmail.com>
 */
#ifdef HAVE_ATOMESE

#include <cstdlib>
#include <opencog/atomspace/AtomSpace.h>
#include <opencog/persist/api/StorageNode.h>
#include <opencog/persist/cog-storage/CogStorage.h>
#include <opencog/persist/file/FileStorage.h>
#include <opencog/persist/rocks/RocksStorage.h>
#include <opencog/persist/sexpr/Sexpr.h>
#include <opencog/nlp/types/atom_types.h>

#undef STRINGIFY

extern "C" {
#include "../link-includes.h"            // For Dictionary
#include "../dict-common/dict-common.h"  // for Dictionary_s
#include "../dict-common/dict-utils.h"   // for size_of_expression()
#include "../dict-ram/dict-ram.h"
#include "lookup-atomese.h"
};

#include "dict-atomese.h"
#include "local-as.h"

using namespace opencog;

// Strings we expect to find in the dictionary.
#define STORAGE_NODE_STRING "storage-node"
#define COST_KEY_STRING "cost-key"
#define COST_INDEX_STRING "cost-index"
#define COST_SCALE_STRING "cost-scale"
#define COST_OFFSET_STRING "cost-offset"
#define COST_CUTOFF_STRING "cost-cutoff"
#define COST_DEFAULT_STRING "cost-default"

#define PAIR_KEY_STRING "pair-key"
#define PAIR_INDEX_STRING "pair-index"
#define PAIR_SCALE_STRING "pair-scale"
#define PAIR_OFFSET_STRING "pair-offset"
#define PAIR_CUTOFF_STRING "pair-cutoff"
#define PAIR_DEFAULT_STRING "pair-default"

/// Shared global
static AtomSpacePtr external_atomspace;
static StorageNodePtr external_storage;

void lg_config_atomspace(AtomSpacePtr asp, StorageNodePtr sto)
{
	external_atomspace = asp;
	external_storage = sto;
}

static const char* get_dict_define(Dictionary dict, const char* namestr)
{
	const char* val_str =
		linkgrammar_get_dict_define(dict, namestr);
	if (nullptr == val_str) return nullptr;

	// Brute-force unescape quotes. Simple, dumb.
	char* unescaped = (char*) alloca(strlen(val_str)+1);
	const char* p = val_str;
	char* q = unescaped;
	while (*p) { if ('\\' != *p) { *q = *p; q++; } p++; }
	*q = 0x0;

	return string_set_add(unescaped, dict->string_set);
}

/// Open a connection to a StorageNode.
bool as_open(Dictionary dict)
{
	const char * stns = get_dict_define(dict, STORAGE_NODE_STRING);
	if (nullptr == stns) return false;
	dict->name = stns;

	Local* local = new Local;
	local->node_str = stns;

	// If an external atomspace is specified, then use that.
	if (external_atomspace)
	{
		local->asp = external_atomspace;
		Handle hsn = local->asp->add_atom(external_storage);
		local->stnp = StorageNodeCast(hsn);
		local->using_external_as = true;
	}
	else
	{
		local->asp = createAtomSpace();
		local->using_external_as = false;
	}

	// Create the connector predicate.
	// This will be used to cache LG connector strings.
	local->linkp = local->asp->add_node(PREDICATE_NODE,
		"*-LG connector string-*");

	// local->djp = local->asp->add_node(PREDICATE_NODE,
	//	"*-LG disjunct string-*");

	// Marks word-pairs.
	local->lany = local->asp->add_node(LG_LINK_NODE, "ANY");

	// Costs are assumed to be minus the MI located at some key.
	const char* miks = get_dict_define(dict, COST_KEY_STRING);
	Handle mikh = Sexpr::decode_atom(miks);
	local->miks = local->asp->add_atom(mikh);

	const char* mikp = get_dict_define(dict, PAIR_KEY_STRING);
	Handle miki = Sexpr::decode_atom(mikp);
	local->mikp = local->asp->add_atom(miki);

#define LDEF(NAME) linkgrammar_get_dict_define(dict, NAME)

	local->cost_index = atoi(LDEF(COST_INDEX_STRING));
	local->cost_scale = atof(LDEF(COST_SCALE_STRING));
	local->cost_offset = atof(LDEF(COST_OFFSET_STRING));
	local->cost_cutoff = atof(LDEF(COST_CUTOFF_STRING));
	local->cost_default = atof(LDEF(COST_DEFAULT_STRING));

	local->pair_index = atoi(LDEF(PAIR_INDEX_STRING));
	local->pair_scale = atof(LDEF(PAIR_SCALE_STRING));
	local->pair_offset = atof(LDEF(PAIR_OFFSET_STRING));
	local->pair_cutoff = atof(LDEF(PAIR_CUTOFF_STRING));
	local->pair_default = atof(LDEF(PAIR_DEFAULT_STRING));

	dict->as_server = (void*) local;

	if (local->using_external_as) return true;

	// --------------------
	// If we are here, then we manage our own private AtomSpace.

	if (external_storage)
	{
		Handle hsn = local->asp->add_atom(external_storage);
		local->stnp = StorageNodeCast(hsn);
	}
	else
	{
		Handle hsn = Sexpr::decode_atom(local->node_str);
		hsn = local->asp->add_atom(hsn);
		local->stnp = StorageNodeCast(hsn);
	}

	std::string stone = local->stnp->to_short_string();
	const char * stoname = stone.c_str();

#define SHLIB_CTOR_HACK 1
#ifdef SHLIB_CTOR_HACK
	/* The cast below forces the shared lib constructor to run. */
	/* That's needed to force the factory to get installed. */
	/* We need a more elegant solution to this. */
	Type snt = local->stnp->get_type();
	if (COG_STORAGE_NODE == snt)
		local->stnp = CogStorageNodeCast(local->stnp);
	else if (ROCKS_STORAGE_NODE == snt)
		local->stnp = RocksStorageNodeCast(local->stnp);
	else if (FILE_STORAGE_NODE == snt)
		local->stnp = FileStorageNodeCast(local->stnp);
	else
		printf("Unknown storage %s\n", stoname);
#endif

	local->stnp->open();

	// XXX FIXME -- if we cannot connect, then should hard-fail.
	if (local->stnp->connected())
		printf("Connected to %s\n", stoname);
	else
		printf("Failed to connect to %s\n", stoname);

	return true;
}

/// Close the connection to the StorageNode (e.g. cogserver.)
/// To be used only if the everything has been fetched, and the
/// dict is now in local RAM. The dict remains usable, after
/// closing the connection. Only local StorageNodes are closed.
/// External storage nodes will remain open, but will no longer
/// be used.
void as_storage_close(Dictionary dict)
{
	if (nullptr == dict->as_server) return;
	Local* local = (Local*) (dict->as_server);

	if (not local->using_external_as and local->stnp)
		local->stnp->close();

	local->stnp = nullptr;
}

/// Close the connection to the AtomSpace. This will also empty out
/// the local dictionary, and so the dictionary will no longer be
/// usable after a close.
void as_close(Dictionary dict)
{
	if (nullptr == dict->as_server) return;
	Local* local = (Local*) (dict->as_server);
	if (not local->using_external_as and local->stnp)
		local->stnp->close();

	delete local;
	dict->as_server = nullptr;

	// Clear the cache as well
	free_dictionary_root(dict);
	dict->num_entries = 0;
}

// ===============================================================

static size_t count_sections(Local* local, const Handle& germ)
{
	// Are there any Sections in the local atomspace?
	size_t nsects = germ->getIncomingSetSizeByType(SECTION);
	if (0 < nsects or nullptr == local->stnp) return nsects;

	local->stnp->fetch_incoming_by_type(germ, SECTION);
	local->stnp->barrier();
	nsects = germ->getIncomingSetSizeByType(SECTION);

	return nsects;
}

/// Return true if the given word can be found in the dictionary,
/// else return false.
bool as_boolean_lookup(Dictionary dict, const char *s)
{
	bool found = dict_node_exists_lookup(dict, s);
	if (found) return true;

	if (0 == strcmp(s, LEFT_WALL_WORD))
		s = "###LEFT-WALL###";

	Local* local = (Local*) (dict->as_server);
	Handle wrd = local->asp->add_node(WORD_NODE, s);

	// Are there any Sections for this word in the local atomspace?
	size_t nwrdsects = count_sections(local, wrd);

	// Does this word belong to any classes?
	size_t nclass = wrd->getIncomingSetSizeByType(MEMBER_LINK);
	if (0 == nclass and local->stnp)
	{
		local->stnp->fetch_incoming_by_type(wrd, MEMBER_LINK);
		local->stnp->barrier();
	}

	size_t nclssects = 0;
	for (const Handle& memb : wrd->getIncomingSetByType(MEMBER_LINK))
	{
		const Handle& wcl = memb->getOutgoingAtom(1);
		if (WORD_CLASS_NODE != wcl->get_type()) continue;
		nclssects += count_sections(local, wcl);
	}

	lgdebug(+D_SPEC+5,
		"as_boolean_lookup for >>%s<< found class=%lu nsects=%lu %lu\n",
		s, nclass, nwrdsects, nclssects);

	return 0 != (nwrdsects + nclssects);
}

// ===============================================================

Exp* make_exprs(Dictionary dict, const Handle& germ)
{
	return make_sect_exprs(dict, germ);
}

Dict_node * as_lookup_list(Dictionary dict, const char *s)
{
	// Do we already have this word cached? If so, pull from
	// the cache.
	Dict_node * dn = dict_node_lookup(dict, s);

	if (dn) return dn;

	const char* ssc = string_set_add(s, dict->string_set);
	Local* local = (Local*) (dict->as_server);

	if (0 == strcmp(s, LEFT_WALL_WORD))
		s = "###LEFT-WALL###";

	Handle wrd = local->asp->get_node(WORD_NODE, s);
	if (nullptr == wrd) return nullptr;

	// Get expressions, where the word itself is the germ.
	Exp* exp = make_exprs(dict, wrd);

	// Get expressions, where the word is in some class.
	for (const Handle& memb : wrd->getIncomingSetByType(MEMBER_LINK))
	{
		const Handle& wcl = memb->getOutgoingAtom(1);
		if (WORD_CLASS_NODE != wcl->get_type()) continue;

		Exp* clexp = make_exprs(dict, wcl);
		if (nullptr == clexp) continue;

		lgdebug(+D_SPEC+5, "as_lookup_list class for >>%s<< nexpr=%d\n",
			ssc, size_of_expression(clexp));

		if (nullptr == exp)
			exp = clexp;
		else
			exp = make_or_node(dict->Exp_pool, exp, clexp);
	}

	if (nullptr == exp)
		return nullptr;

	dn = (Dict_node*) malloc(sizeof(Dict_node));
	memset(dn, 0, sizeof(Dict_node));
	dn->string = ssc;
	dn->exp = exp;

	// Cache the result; avoid repeated lookups.
	dict->root = dict_node_insert(dict, dict->root, dn);
	dict->num_entries++;

	lgdebug(+D_SPEC+5, "as_lookup_list %d for >>%s<< nexpr=%d\n",
		dict->num_entries, ssc, size_of_expression(exp));

	// Rebalance the tree every now and then.
	if (0 == dict->num_entries% 30)
	{
		dict->root = dsw_tree_to_vine(dict->root);
		dict->root = dsw_vine_to_tree(dict->root, dict->num_entries);
	}

	// Perform the lookup. We cannot return the dn above, as the
	// as_free_llist() below will delete it, leading to mem corruption.
	dn = dict_node_lookup(dict, ssc);
	return dn;
}

// This is supposed to provide a wild-card lookup.  However,
// There is currently no way to support a wild-card lookup in the
// atomspace: there is no way to ask for all WordNodes that match
// a given regex.  There's no regex predicate... this can be hacked
// around in various elegant and inelegant ways, e.g. adding a
// regex predicate to the AtomSpace. Punt for now. This is used
// only for the !! command in the parser command-line tool.
// XXX FIXME. But low priority.
Dict_node * as_lookup_wild(Dictionary dict, const char *s)
{
	Dict_node * dn = dict_node_wild_lookup(dict, s);
	if (dn) return dn;

	as_lookup_list(dict, s);
	return dict_node_wild_lookup(dict, s);
}

// Zap all the Dict_nodes that we've added earlier.
// This clears out everything hanging on dict->root
// as well as the expression pool.
// And also the local AtomSpace.
//
void as_clear_cache(Dictionary dict)
{
	Local* local = (Local*) (dict->as_server);
	printf("Prior to clear, dict has %d entries, Atomspace has %lu Atoms\n",
		dict->num_entries, local->asp->get_size());

	dict->Exp_pool = pool_new(__func__, "Exp", /*num_elements*/4096,
	                             sizeof(Exp), /*zero_out*/false,
	                             /*align*/false, /*exact*/false);

	// Clear the local AtomSpace too.
	// Easiest way to do this is to just close and reopen
	// the connection.

	AtomSpacePtr savea = external_atomspace;
	StorageNodePtr saves = external_storage;
	if (local->using_external_as)
	{
		external_atomspace = local->asp;
		external_storage = local->stnp;
	}

	as_close(dict);
	as_open(dict);
	external_atomspace = savea;
	external_storage = saves;
	as_boolean_lookup(dict, LEFT_WALL_WORD);
}
#endif /* HAVE_ATOMESE */
