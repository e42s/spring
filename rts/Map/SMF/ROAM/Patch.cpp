/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

//
// ROAM Simplistic Implementation
// Added to Spring by Peter Sarkozy (mysterme AT gmail DOT com)
// Billion thanks to Bryan Turner (Jan, 2000)
//                    brturn@bellsouth.net
//
// Based on the Tread Marks engine by Longbow Digital Arts
//                               (www.LongbowDigitalArts.com)
// Much help and hints provided by Seumas McNally, LDA.
//

#include "Patch.h"
#include "RoamMeshDrawer.h"
#include "Map/ReadMap.h"
#include "Map/SMF/SMFGroundDrawer.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/VertexArray.h"
#include "System/Log/ILog.h"
#include "System/Threading/ThreadPool.h"

#include <climits>


Patch::RenderMode Patch::renderMode = Patch::VBO;

static size_t CUR_POOL_SIZE =                 0; // split over all threads
static size_t MAX_POOL_SIZE = NEW_POOL_SIZE * 8; // upper limit for ResetAll


static std::vector<CTriNodePool> pools[CRoamMeshDrawer::MESH_COUNT];


void CTriNodePool::InitPools(bool shadowPass, size_t newPoolSize)
{
	const int numThreads = ThreadPool::GetMaxThreads();
	const size_t thrPoolSize = std::max((CUR_POOL_SIZE = newPoolSize) / numThreads, newPoolSize / 3);

	try {
		pools[shadowPass].clear();
		pools[shadowPass].reserve(numThreads);

		for (int i = 0; i < numThreads; i++) {
			pools[shadowPass].emplace_back(thrPoolSize + (thrPoolSize & 1));
		}
	} catch (const std::bad_alloc& e) {
		LOG_L(L_FATAL, "[TriNodePool::%s] bad_alloc exception \"%s\" (numThreads=%d newPoolSize=%lu)", __func__, e.what(), numThreads, (unsigned long) newPoolSize);

		// try again after reducing the wanted pool-size by a quarter
		InitPools(shadowPass, MAX_POOL_SIZE = (newPoolSize - (newPoolSize >> 2)));
	}
}

void CTriNodePool::ResetAll(bool shadowPass)
{
	bool outOfNodes = false;

	for (CTriNodePool& pool: pools[shadowPass]) {
		outOfNodes |= pool.OutOfNodes();
		pool.Reset();
	}

	if (!outOfNodes)
		return;
	if (CUR_POOL_SIZE >= MAX_POOL_SIZE)
		return;

	InitPools(shadowPass, std::min<size_t>(CUR_POOL_SIZE * 2, MAX_POOL_SIZE));
}


CTriNodePool* CTriNodePool::GetPool(bool shadowPass)
{
	return &(pools[shadowPass][ThreadPool::GetThreadNum()]);
}


CTriNodePool::CTriNodePool(const size_t poolSize): nextTriNodeIdx(0)
{
	// child nodes are always allocated in pairs, so poolSize must be even
	// (it does not technically need to be non-zero since patch root nodes
	// live outside the pool, but KISS)
	assert((poolSize & 0x1) == 0);
	assert(poolSize > 0);

	pool.resize(poolSize);
}


void CTriNodePool::Reset()
{
	// reinit all entries; faster than calling TriTreeNode's ctor
	if (nextTriNodeIdx > 0)
		memset(&pool[0], 0, sizeof(TriTreeNode) * nextTriNodeIdx);

	nextTriNodeIdx = 0;
}

bool CTriNodePool::Allocate(TriTreeNode*& left, TriTreeNode*& right)
{
	// pool exhausted, make sure both child nodes are NULL
	if (OutOfNodes()) {
		left  = nullptr;
		right = nullptr;
		return false;
	}

	left  = &(pool[nextTriNodeIdx++]);
	right = &(pool[nextTriNodeIdx++]);
	return true;
}




Patch::Patch()
	: smfGroundDrawer(nullptr)
	, currentVariance(nullptr)
	, currentPool(nullptr)
	, isDirty(true)
	, vboVerticesUploaded(false)
	, varianceMaxLimit(std::numeric_limits<float>::max())
	, camDistLODFactor(1.0f)
	, coors(-1, -1)
	, triList(0)
	, vertexBuffer(0)
	, vertexIndexBuffer(0)
{
	varianceLeft.resize(1 << VARIANCE_DEPTH);
	varianceRight.resize(1 << VARIANCE_DEPTH);
}

Patch::~Patch()
{
	glDeleteLists(triList, 1);

	if (GLEW_ARB_vertex_buffer_object) {
		glDeleteBuffers(1, &vertexBuffer);
		glDeleteBuffers(1, &vertexIndexBuffer);
	}

	triList = 0;
	vertexBuffer = 0;
	vertexIndexBuffer = 0;
}

void Patch::Init(CSMFGroundDrawer* _drawer, int patchX, int patchZ)
{
	coors.x = patchX;
	coors.y = patchZ;

	smfGroundDrawer = _drawer;

	// attach the two base-triangles together
	baseLeft.BaseNeighbor  = &baseRight;
	baseRight.BaseNeighbor = &baseLeft;

	// create used OpenGL objects
	triList = glGenLists(1);

	if (GLEW_ARB_vertex_buffer_object) {
		glGenBuffers(1, &vertexBuffer);
		glGenBuffers(1, &vertexIndexBuffer);
	}


	vertices.resize(3 * (PATCH_SIZE + 1) * (PATCH_SIZE + 1));
	unsigned int index = 0;

	// initialize vertices
	for (int z = coors.y; z <= (coors.y + PATCH_SIZE); z++) {
		for (int x = coors.x; x <= (coors.x + PATCH_SIZE); x++) {
			vertices[index++] = x * SQUARE_SIZE;
			vertices[index++] = 0.0f;
			vertices[index++] = z * SQUARE_SIZE;
		}
	}

	UpdateHeightMap();
}

void Patch::Reset()
{
	// reset the important relationships
	baseLeft  = TriTreeNode();
	baseRight = TriTreeNode();

	// attach the two base-triangles together
	baseLeft.BaseNeighbor  = &baseRight;
	baseRight.BaseNeighbor = &baseLeft;
}


void Patch::UpdateHeightMap(const SRectangle& rect)
{
	const float* hMap = readMap->GetCornerHeightMapUnsynced();

	for (int z = rect.z1; z <= rect.z2; z++) {
		for (int x = rect.x1; x <= rect.x2; x++) {
			const int vindex = (z * (PATCH_SIZE + 1) + x) * 3;

			const int xw = x + coors.x;
			const int zw = z + coors.y;

			// only update y-coord
			vertices[vindex + 1] = hMap[zw * mapDims.mapxp1 + xw];
		}
	}

	VBOUploadVertices();
	isDirty = true;
}


void Patch::VBOUploadVertices()
{
	if (renderMode == VBO) {
		// Upload vertexBuffer
		glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), &vertices[0], GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		vboVerticesUploaded = true;
	} else {
		vboVerticesUploaded = false;
	}
}


// -------------------------------------------------------------------------------------------------
// Split a single Triangle and link it into the mesh.
// Will correctly force-split diamonds.
//
bool Patch::Split(TriTreeNode* tri)
{
	// we are already split, no need to do it again
	if (!tri->IsLeaf())
		return true;

	// if this triangle is not in a proper diamond, force split our base neighbor
	if (tri->BaseNeighbor != nullptr && (tri->BaseNeighbor->BaseNeighbor != tri))
		Split(tri->BaseNeighbor);

	// create children and link into mesh, or make this triangle a leaf
	if (!currentPool->Allocate(tri->LeftChild, tri->RightChild))
		return false;

	assert(tri->IsBranch());

	// fill in the information we can get from the parent (neighbor pointers)
	tri->LeftChild->BaseNeighbor = tri->LeftNeighbor;
	tri->LeftChild->LeftNeighbor = tri->RightChild;

	tri->RightChild->BaseNeighbor = tri->RightNeighbor;
	tri->RightChild->RightNeighbor = tri->LeftChild;

	// link our left-neighbor to the new children
	if (tri->LeftNeighbor != nullptr) {
		if (tri->LeftNeighbor->BaseNeighbor == tri)
			tri->LeftNeighbor->BaseNeighbor = tri->LeftChild;
		else if (tri->LeftNeighbor->LeftNeighbor == tri)
			tri->LeftNeighbor->LeftNeighbor = tri->LeftChild;
		else if (tri->LeftNeighbor->RightNeighbor == tri)
			tri->LeftNeighbor->RightNeighbor = tri->LeftChild;
		else
			;// illegal Left neighbor
	}

	// link our right-neighbor to the new children
	if (tri->RightNeighbor != nullptr) {
		if (tri->RightNeighbor->BaseNeighbor == tri)
			tri->RightNeighbor->BaseNeighbor = tri->RightChild;
		else if (tri->RightNeighbor->RightNeighbor == tri)
			tri->RightNeighbor->RightNeighbor = tri->RightChild;
		else if (tri->RightNeighbor->LeftNeighbor == tri)
			tri->RightNeighbor->LeftNeighbor = tri->RightChild;
		else
			;// illegal Right neighbor
	}

	// link our base-neighbor to the new children
	if (tri->BaseNeighbor != nullptr) {
		if (tri->BaseNeighbor->IsBranch()) {
			tri->BaseNeighbor->LeftChild->RightNeighbor = tri->RightChild;
			tri->BaseNeighbor->RightChild->LeftNeighbor = tri->LeftChild;

			tri->LeftChild->RightNeighbor = tri->BaseNeighbor->RightChild;
			tri->RightChild->LeftNeighbor = tri->BaseNeighbor->LeftChild;
		} else {
			// base Neighbor (in a diamond with us) was not split yet, do so now
			Split(tri->BaseNeighbor);
		}
	} else {
		// edge triangle, trivial case
		tri->LeftChild->RightNeighbor = nullptr;
		tri->RightChild->LeftNeighbor = nullptr;
	}

	return true;
}


// ---------------------------------------------------------------------
// Tessellate a Patch.
// Will continue to split until the variance metric is met.
//
void Patch::RecursTessellate(TriTreeNode* tri, const int2 left, const int2 right, const int2 apex, const int node)
{
	// bail if we can not tessellate further in at least one dimension
	if ((abs(left.x - right.x) <= 1) && (abs(left.y - right.y) <= 1))
		return;

	// default > 1; when variance isn't saved this issues further tessellation
	float triVariance = 10.0f;

	if (node < (1 << VARIANCE_DEPTH)) {
		// make maximum tessellation-level dependent on camDistLODFactor
		// huge cliffs cause huge variances and would otherwise always tessellate
		// regardless of the actual camera distance (-> huge/distfromcam ~= huge)
		const int sizeX = std::max(left.x - right.x, right.x - left.x);
		const int sizeY = std::max(left.y - right.y, right.y - left.y);
		const int size  = std::max(sizeX, sizeY);

		// take distance, variance and patch size into consideration
		triVariance = (std::min(currentVariance[node], varianceMaxLimit) * PATCH_SIZE * size) * camDistLODFactor;
	}

	// stop tesselation
	if (triVariance <= 1.0f)
		return;

	Split(tri);

	if (tri->IsBranch()) {
		// triangle was split, also try to split its children
		const int2 center = {(left.x + right.x) >> 1, (left.y + right.y) >> 1};

		RecursTessellate(tri->LeftChild,  apex,  left, center, (node << 1)    );
		RecursTessellate(tri->RightChild, right, apex, center, (node << 1) + 1);
	}
}


// ---------------------------------------------------------------------
// Render the tree.
//

void Patch::RecursRender(const TriTreeNode* tri, const int2 left, const int2 right, const int2 apex)
{
	if (tri->IsLeaf()) {
		indices.push_back(apex.x  + apex.y  * (PATCH_SIZE + 1));
		indices.push_back(left.x  + left.y  * (PATCH_SIZE + 1));
		indices.push_back(right.x + right.y * (PATCH_SIZE + 1));
		return;
	}

	const int2 center = {(left.x + right.x) >> 1, (left.y + right.y) >> 1};

	RecursRender(tri->LeftChild,  apex,  left, center);
	RecursRender(tri->RightChild, right, apex, center);
}


void Patch::GenerateIndices()
{
	indices.clear();
	RecursRender(&baseLeft,  int2(         0, PATCH_SIZE), int2(PATCH_SIZE,          0), int2(         0,          0));
	RecursRender(&baseRight, int2(PATCH_SIZE,          0), int2(         0, PATCH_SIZE), int2(PATCH_SIZE, PATCH_SIZE));
}

float Patch::GetHeight(int2 pos)
{
	const int vindex = (pos.y * (PATCH_SIZE + 1) + pos.x) * 3 + 1;
	assert(readMap->GetCornerHeightMapUnsynced()[(coors.y + pos.y) * mapDims.mapxp1 + (coors.x + pos.x)] == vertices[vindex]);
	return vertices[vindex];
}

// ---------------------------------------------------------------------
// Computes Variance over the entire tree.  Does not examine node relationships.
//
float Patch::RecursComputeVariance(
	const   int2 left,
	const   int2 rght,
	const   int2 apex,
	const float3 hgts,
	const    int node
) {
	/*      A
	 *     /|\
	 *    / | \
	 *   /  |  \
	 *  /   |   \
	 * L----M----R
	 *
	 * first compute the XZ coordinates of 'M' (hypotenuse middle)
	 */
	const int2 mpos = {(left.x + rght.x) >> 1, (left.y + rght.y) >> 1};

	// get the height value at M
	const float mhgt = GetHeight(mpos);

	// variance of this triangle is the actual height at its hypotenuse
	// midpoint minus the interpolated height; use values passed on the
	// stack instead of re-accessing the heightmap
	float myVariance = math::fabs(mhgt - ((hgts.x + hgts.y) * 0.5f));

	// shore lines get more variance for higher accuracy
	// NOTE: .x := height(L), .y := height(R), .z := height(A)
	//
	if ((hgts.x * hgts.y) < 0.0f || (hgts.x * mhgt) < 0.0f || (hgts.y * mhgt) < 0.0f)
		myVariance = std::max(myVariance * 1.5f, 20.0f);

	// myVariance = MAX(abs(left.x - rght.x), abs(left.y - rght.y)) * myVariance;

	// save some CPU, only calculate variance down to a 4x4 block
	if ((abs(left.x - rght.x) >= 4) || (abs(left.y - rght.y) >= 4)) {
		const float3 hgts1 = {hgts.z, hgts.x, mhgt};
		const float3 hgts2 = {hgts.y, hgts.z, mhgt};

		const float child1Variance = RecursComputeVariance(apex, left, mpos, hgts1, (node << 1)    );
		const float child2Variance = RecursComputeVariance(rght, apex, mpos, hgts2, (node << 1) + 1);

		// final variance for this node is the max of its own variance and that of its children
		myVariance = std::max(myVariance, child1Variance);
		myVariance = std::max(myVariance, child2Variance);
	}

	// NOTE: Variance is never zero
	myVariance = std::max(0.001f, myVariance);

	// store the final variance for this node
	if (node < (1 << VARIANCE_DEPTH))
		currentVariance[node] = myVariance;

	return myVariance;
}


// ---------------------------------------------------------------------
// Compute the variance tree for each of the Binary Triangles in this patch.
//
void Patch::ComputeVariance()
{
	{
		currentVariance = &varianceLeft[0];

		const   int2 left = {         0, PATCH_SIZE};
		const   int2 rght = {PATCH_SIZE,          0};
		const   int2 apex = {         0,          0};
		const float3 hgts = {
			GetHeight(left),
			GetHeight(rght),
			GetHeight(apex),
		};

		RecursComputeVariance(left, rght, apex, hgts, 1);
	}

	{
		currentVariance = &varianceRight[0];

		const   int2 left = {PATCH_SIZE,          0};
		const   int2 rght = {         0, PATCH_SIZE};
		const   int2 apex = {PATCH_SIZE, PATCH_SIZE};
		const float3 hgts = {
			GetHeight(left),
			GetHeight(rght),
			GetHeight(apex),
		};

		RecursComputeVariance(left, rght, apex, hgts, 1);
	}

	// Clear the dirty flag for this patch
	isDirty = false;
}


// ---------------------------------------------------------------------
// Create an approximate mesh.
//
bool Patch::Tessellate(const float3& camPos, int viewRadius, bool shadowPass)
{
	// Set/Update LOD params (FIXME: wrong height?)
	float3 midPos;
	midPos.x = (coors.x + PATCH_SIZE / 2) * SQUARE_SIZE;
	midPos.z = (coors.y + PATCH_SIZE / 2) * SQUARE_SIZE;
	midPos.y = (readMap->GetCurrMinHeight() + readMap->GetCurrMaxHeight()) * 0.5f;

	currentPool = CTriNodePool::GetPool(shadowPass);

	camDistLODFactor  = midPos.distance(camPos);
	camDistLODFactor *= (300.0f / viewRadius); // MAGIC NUMBER 1: increase the dividend to reduce LOD in camera distance
	camDistLODFactor  = std::max(1.0f, camDistLODFactor);
	camDistLODFactor  = 1.0f / camDistLODFactor;

	// MAGIC NUMBER 2:
	//   variances are clamped by it, so it regulates how strong areas are tessellated.
	//   Note, the maximum tessellation is untouched by it. Instead it reduces the maximum
	//   LOD in distance, while the param above defines the overall FallOff rate.
	varianceMaxLimit = viewRadius * 0.35f;

	{
		// Split each of the base triangles
		currentVariance = &varianceLeft[0];

		const int2 left = {coors.x,              coors.y + PATCH_SIZE};
		const int2 rght = {coors.x + PATCH_SIZE, coors.y             };
		const int2 apex = {coors.x,              coors.y             };

		RecursTessellate(&baseLeft, left, rght, apex, 1);
	}
	{
		currentVariance = &varianceRight[0];

		const int2 left = {coors.x + PATCH_SIZE, coors.y             };
		const int2 rght = {coors.x,              coors.y + PATCH_SIZE};
		const int2 apex = {coors.x + PATCH_SIZE, coors.y + PATCH_SIZE};

		RecursTessellate(&baseRight, left, rght, apex, 1);
	}

	return (!currentPool->OutOfNodes());
}


// ---------------------------------------------------------------------
// Render the mesh.
//

void Patch::Draw()
{
	switch (renderMode) {
		case VA: {
			glEnableClientState(GL_VERTEX_ARRAY);
				glVertexPointer(3, GL_FLOAT, 0, &vertices[0]);
				glDrawRangeElements(GL_TRIANGLES, 0, vertices.size(), indices.size(), GL_UNSIGNED_INT, &indices[0]);
			glDisableClientState(GL_VERTEX_ARRAY);
		} break;

		case DL: {
			glCallList(triList);
		} break;

		case VBO: {
			// enable VBOs
			glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer); // coors
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertexIndexBuffer); // indices

				glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(3, GL_FLOAT, 0, 0); // last param is offset, not ptr
					glDrawRangeElements(GL_TRIANGLES, 0, vertices.size(), indices.size(), GL_UNSIGNED_INT, 0);
				glDisableClientState(GL_VERTEX_ARRAY);

			// disable VBO mode
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		} break;

		default: {
			assert(false);
		} break;
	}
}


void Patch::DrawBorder()
{
	CVertexArray* va = GetVertexArray();
	GenerateBorderIndices(va);
	va->DrawArrayC(GL_TRIANGLES);
}


void Patch::RecursBorderRender(
	CVertexArray* va,
	const TriTreeNode* tri,
	const int2 left,
	const int2 rght,
	const int2 apex,
	int depth,
	bool leftChild
) {
	if (tri->IsLeaf()) {
		const float3& v1 = *(float3*)&vertices[(apex.x + apex.y * (PATCH_SIZE + 1))*3];
		const float3& v2 = *(float3*)&vertices[(left.x + left.y * (PATCH_SIZE + 1))*3];
		const float3& v3 = *(float3*)&vertices[(rght.x + rght.y * (PATCH_SIZE + 1))*3];

		static constexpr unsigned char white[] = {255, 255, 255, 255};
		static constexpr unsigned char trans[] = {255, 255, 255,   0};

		va->EnlargeArrays(6, 0, VA_SIZE_C);

		if ((depth & 1) == 0) {
			va->AddVertexQC(v2,                          white);
			va->AddVertexQC(float3(v2.x, -400.0f, v2.z), trans);
			va->AddVertexQC(float3(v3.x, v3.y, v3.z),    white);

			va->AddVertexQC(v3,                          white);
			va->AddVertexQC(float3(v2.x, -400.0f, v2.z), trans);
			va->AddVertexQC(float3(v3.x, -400.0f, v3.z), trans);
		} else {
			if (leftChild) {
				va->AddVertexQC(v1,                          white);
				va->AddVertexQC(float3(v1.x, -400.0f, v1.z), trans);
				va->AddVertexQC(float3(v2.x, v2.y, v2.z),    white);

				va->AddVertexQC(v2,                          white);
				va->AddVertexQC(float3(v1.x, -400.0f, v1.z), trans);
				va->AddVertexQC(float3(v2.x, -400.0f, v2.z), trans);
			} else {
				va->AddVertexQC(v3,                          white);
				va->AddVertexQC(float3(v3.x, -400.0f, v3.z), trans);
				va->AddVertexQC(float3(v1.x, v1.y, v1.z),    white);

				va->AddVertexQC(v1,                          white);
				va->AddVertexQC(float3(v3.x, -400.0f, v3.z), trans);
				va->AddVertexQC(float3(v1.x, -400.0f, v1.z), trans);
			}
		}

		return;
	}

	const int2 center = {(left.x + rght.x) >> 1, (left.y + rght.y) >> 1};

	// at even depths, descend down left *and* right children since both
	// are on the patch-edge; returns are needed for gcc's TCO (although
	// unlikely to be applied)
	if ((depth & 1) == 0) {
		       RecursBorderRender(va, tri->LeftChild,  apex, left, center, depth + 1, !leftChild);
		return RecursBorderRender(va, tri->RightChild, rght, apex, center, depth + 1,  leftChild);
	}

	// at odd depths (where only one triangle is on the edge), always force
	// a left-bias for the next call so the recursion ends up at the correct
	// leafs
	if (leftChild) {
		return RecursBorderRender(va, tri->LeftChild,  apex, left, center, depth + 1, true);
	} else {
		return RecursBorderRender(va, tri->RightChild, rght, apex, center, depth + 1, true);
	}
}

void Patch::GenerateBorderIndices(CVertexArray* va)
{
	va->Initialize();

	#define PS PATCH_SIZE
	// border vertices are always part of base-level triangles
	// that have either no left or no right neighbor, i.e. are
	// on the map edge
	if (baseLeft.LeftNeighbor   == nullptr) RecursBorderRender(va, &baseLeft , { 0, PS}, {PS,  0}, { 0,  0}, 1,  true); // left border
	if (baseLeft.RightNeighbor  == nullptr) RecursBorderRender(va, &baseLeft , { 0, PS}, {PS,  0}, { 0,  0}, 1, false); // right border
	if (baseRight.RightNeighbor == nullptr) RecursBorderRender(va, &baseRight, {PS,  0}, { 0, PS}, {PS, PS}, 1, false); // bottom border
	if (baseRight.LeftNeighbor  == nullptr) RecursBorderRender(va, &baseRight, {PS,  0}, { 0, PS}, {PS, PS}, 1,  true); // top border
	#undef PS
}


void Patch::Upload()
{
	switch (renderMode) {
		case DL: {
			glNewList(triList, GL_COMPILE);
				glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(3, GL_FLOAT, 0, &vertices[0]);
					glDrawRangeElements(GL_TRIANGLES, 0, vertices.size(), indices.size(), GL_UNSIGNED_INT, &indices[0]);
				glDisableClientState(GL_VERTEX_ARRAY);
			glEndList();
		} break;

		case VBO: {
			if (!vboVerticesUploaded)
				VBOUploadVertices();

			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertexIndexBuffer);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), &indices[0], GL_DYNAMIC_DRAW);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		} break;

		default: {
		} break;
	}
}

void Patch::SetSquareTexture() const
{
	smfGroundDrawer->SetupBigSquare(coors.x / PATCH_SIZE, coors.y / PATCH_SIZE);
}


void Patch::SwitchRenderMode(int mode)
{
	if (mode < 0) {
		mode = renderMode + 1;
		mode %= 3;
	}

	if (!GLEW_ARB_vertex_buffer_object && mode == VBO)
		mode = DL;

	if (mode == renderMode)
		return;

	switch (mode) {
		case VA: {
			LOG("Set ROAM mode to VA");
			renderMode = VA;
		} break;
		case DL: {
			LOG("Set ROAM mode to DisplayLists");
			renderMode = DL;
		} break;
		case VBO: {
			LOG("Set ROAM mode to VBO");
			renderMode = VBO;
		} break;
	}

	CRoamMeshDrawer::ForceTesselation();
}



// ---------------------------------------------------------------------
// Visibility Update Functions
//

#if 0
void Patch::UpdateVisibility(CCamera* cam)
{
	const float3 mins( coors.x               * SQUARE_SIZE, readMap->GetCurrMinHeight(),  coors.y               * SQUARE_SIZE);
	const float3 maxs((coors.x + PATCH_SIZE) * SQUARE_SIZE, readMap->GetCurrMaxHeight(), (coors.y + PATCH_SIZE) * SQUARE_SIZE);

	if (!cam->InView(mins, maxs))
		return;

	lastDrawFrames[cam->GetCamType()] = globalRendering->drawFrame;
}
#endif


class CPatchInViewChecker : public CReadMap::IQuadDrawer
{
public:
	void ResetState() {}
	void ResetState(CCamera* c = nullptr, Patch* p = nullptr, int xsize = 0) {
		testCamera = c;
		patchArray = p;
		numPatchesX = xsize;
	}

	void DrawQuad(int x, int y) {
		patchArray[y * numPatchesX + x].lastDrawFrames[testCamera->GetCamType()] = globalRendering->drawFrame;
	}

private:
	CCamera* testCamera;
	Patch* patchArray;

	int numPatchesX;
};


void Patch::UpdateVisibility(CCamera* cam, std::vector<Patch>& patches, const int numPatchesX)
{
	#if 0
	// very slow
	for (Patch& p: patches) {
		p.UpdateVisibility(cam);
	}
	#else
	// very fast
	static CPatchInViewChecker checker;

	assert(cam->GetCamType() < CCamera::CAMTYPE_VISCUL);
	checker.ResetState(cam, &patches[0], numPatchesX);

	cam->GetFrustumSides(readMap->GetCurrMinHeight() - 100.0f, readMap->GetCurrMaxHeight() + 100.0f, SQUARE_SIZE);
	readMap->GridVisibility(cam, &checker, 1e9, PATCH_SIZE);
	#endif
}

bool Patch::IsVisible(const CCamera* cam) const {
	return (lastDrawFrames[cam->GetCamType()] >= globalRendering->drawFrame);
}

