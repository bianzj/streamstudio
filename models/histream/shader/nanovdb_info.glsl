#ifndef NANOVDB_INFO
#define NANOVDB_INFO

#include "PnanoVDB.h"


struct NanoVDBLink
{
	pnanovdb_buf_t buf;
	pnanovdb_readaccessor_t accessor;
	pnanovdb_grid_handle_t grid;
	pnanovdb_tree_handle_t tree;
	pnanovdb_root_handle_t root;
};

// Initialization the nanovdb structure for fetching data from buf
NanoVDBLink nanovdbInit() {

	NanoVDBLink nanoVDBLink;
	pnanovdb_grid_handle_t nullgrid = { pnanovdb_address_null() };
	nanoVDBLink.tree = pnanovdb_grid_get_tree(nanoVDBLink.buf, nullgrid);
	nanoVDBLink.root = pnanovdb_tree_get_root(nanoVDBLink.buf, nanoVDBLink.tree);
	pnanovdb_readaccessor_init(nanoVDBLink.accessor, nanoVDBLink.root);
	return nanoVDBLink;
}

// obtain the data using Nanovdb structure and 3D position.
// The link is inout: the read accessor caches the leaf/node path of the last
// lookup, and consecutive cells of a DDA (or consecutive hits of a ray) are
// neighbours. A by-value link discarded that cache after every call, so each
// lookup walked the tree from the root again (the periodic boundary walks
// issue several times more lookups per ray). The value read is unchanged.
int nanovdbGet(inout NanoVDBLink nanoVDBLink, ivec3 pos)
{
	pnanovdb_coord_t ijk =pos;
	pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_INT32, nanoVDBLink.buf, nanoVDBLink.accessor, ijk);
	//uint data = pnanovdb_read_uint32(nanoVDBLink.buf, address);
	int data = pnanovdb_read_int32(nanoVDBLink.buf,address);
	return data;
}



#endif
