// ==========================================================================
// 文件：nanovdb_info.glsl
// 功能：对 PnanoVDB（NanoVDB 的 GLSL 移植）的薄封装，用于在着色器里按整数体元坐标读取
//       稀疏体元网格中存储的 int32 值（例如体元编号/索引）。
// 主要模块：
//   1. NanoVDBLink：保存缓冲 buf、读访问器 accessor 与 grid/tree/root 句柄
//   2. nanovdbInit：由缓冲初始化 tree/root 句柄与读访问器
//   3. nanovdbGet：按 ivec3 坐标取 int32 值（inout 以复用访问器缓存）
// 数据流：宿主端构建的 NanoVDB 网格以 uint 数组上传（bindingLayout.h 中的 B_NANO，
//         pnanovdb_buf_data）→ 本文件按坐标查询。
// 与其他文件关系：依赖 PnanoVDB.h；bindingLayout.h 声明缓冲；调用方在体元 DDA 遍历中使用。
// 约定：坐标为整数体元下标（ijk）；网格类型固定为 INT32（PNANOVDB_GRID_TYPE_INT32）。
// ==========================================================================

#ifndef NANOVDB_INFO
#define NANOVDB_INFO

#include "PnanoVDB.h"


// 模块：NanoVDB 查询句柄。buf 为缓冲，accessor 为带缓存的只读访问器，
//       grid/tree/root 为网格层级句柄。
struct NanoVDBLink
{
	pnanovdb_buf_t buf;
	pnanovdb_readaccessor_t accessor;
	pnanovdb_grid_handle_t grid;
	pnanovdb_tree_handle_t tree;
	pnanovdb_root_handle_t root;
};

// 作用：初始化 NanoVDBLink。网格头位于缓冲起始地址（pnanovdb_address_null()），
//       由此取 tree 与 root，并初始化读访问器。
// 说明：这里未显式给 buf 赋值，依赖 PnanoVDB 在 GLSL 下以全局缓冲 pnanovdb_buf_data 访问（待核对）。
// Initialization the nanovdb structure for fetching data from buf
NanoVDBLink nanovdbInit() {

	NanoVDBLink nanoVDBLink;
	pnanovdb_grid_handle_t nullgrid = { pnanovdb_address_null() };
	nanoVDBLink.tree = pnanovdb_grid_get_tree(nanoVDBLink.buf, nullgrid);
	nanoVDBLink.root = pnanovdb_tree_get_root(nanoVDBLink.buf, nanoVDBLink.tree);
	pnanovdb_readaccessor_init(nanoVDBLink.accessor, nanoVDBLink.root);
	return nanoVDBLink;
}

// 作用：读取体元坐标 pos 处的 int32 值。注意 link 为 inout，用于保留访问器缓存（见下方原注释）。
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
