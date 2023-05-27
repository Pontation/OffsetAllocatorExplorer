#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "OffsetAllocator/offsetAllocator.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/GL.h>
#include <tchar.h>

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <vector>
#include <memory>
#include <format>

namespace OffsetAllocator 
{
namespace SmallFloat
{
uint32 floatToUint(uint32);
}
}


bool operator==(const OffsetAllocator::Allocation& left, const OffsetAllocator::Allocation& right)
{
	return left.offset == right.offset && left.metadata == right.metadata;
}

ImVec2 operator+(const ImVec2& left, const ImVec2& right)
{
	return ImVec2(left.x + right.x, left.y + right.y);
}

using namespace OffsetAllocator;

struct WGL_WindowData { HDC hDC; };

static HGLRC g_hRC;
static WGL_WindowData g_MainWindow;
static int g_Width;
static int g_Height;
static std::unique_ptr<Allocator> allocator;
static std::vector<OffsetAllocator::Allocation> allocations;
static std::unordered_set<uint32_t> keyDownLastFrame;
static int allocatorSize = 1024;
static int maxAllocs = 128 * 1024;

bool IsPressed(int key)
{
	if (GetKeyState(key) & 0x80 && !ImGui::GetIO().WantCaptureKeyboard)
	{
		bool wasDownLastFrame = keyDownLastFrame.contains(key);
		keyDownLastFrame.insert(key);
		return !wasDownLastFrame;
	}

	keyDownLastFrame.erase(key);
	return false;
}

bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data);
void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void DrawAllocatorNode(ImVec2 pos, uint32_t nodeIndex, uint32_t offset, uint32_t size, ImU32 lineColor, ImU32 boxColor, ImU32 textColor, ImVec2 boxSize, float rounding, float lineThickness, float margin)
{
	float lineHeight = ImGui::GetTextLineHeight();
	auto drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(pos, pos + boxSize, boxColor, rounding);
	drawList->AddRect(pos, pos + boxSize, lineColor, rounding, 0, lineThickness);
	std::string label = std::format("{}", nodeIndex);
	ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
	ImVec2 textPos = pos + ImVec2((boxSize.x - textSize.x) / 2.0f, 0);
	drawList->AddText(textPos, textColor, label.c_str());
	pos.y += lineHeight + 2;
	drawList->AddLine(pos, pos + ImVec2(boxSize.x, 0), lineColor, lineThickness);
	pos.y += lineThickness;
	drawList->AddText(pos + ImVec2(margin, 0), textColor, std::format("O: {}", offset).c_str());
	pos.y += lineHeight;
	drawList->AddText(pos + ImVec2(margin, 0), textColor, std::format("S: {}", size).c_str());
}

void ShowAllocatorExplorer()
{
	ImGui::Begin("Offset Allocator Explorer");

	if (allocator)
	{
		auto topLevelReport = allocator->storageReport();

		ImGui::Text("Total free space: %d", topLevelReport.totalFreeSpace);
		ImGui::Text("Largest free region: %d", topLevelReport.largestFreeRegion);
		ImGui::NewLine();

		static int allocationSize = 1;
		ImGui::InputInt("Size", &allocationSize);
		ImGui::SameLine();

		if (ImGui::Button("Allocate (A)") || IsPressed('A'))
		{
			auto allocation = allocator->allocate(allocationSize);
			if (allocation.offset != Allocation::NO_SPACE)
				allocations.emplace_back(allocation);
		}
		
		ImGui::NewLine();
		if (ImGui::Button("Clear Allocations (C)") || IsPressed('C'))
		{
			allocations.clear();
			allocator->reset();
		}
		ImGui::SameLine();
		if (ImGui::Button("Destroy Allocator (D)") || IsPressed('D'))
		{
			allocations.clear();
			allocator.reset();
		}
	}
	else
	{
		ImGui::InputInt("Size", &allocatorSize);
		ImGui::InputInt("Max Allocations", &maxAllocs);

		if (ImGui::Button("New Allocator (N)") || IsPressed('N'))
		{
			allocator = std::make_unique<Allocator>(allocatorSize, maxAllocs);
		}
	}

	ImGui::End();

	ImGui::Begin("Visualization");
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	float bytesPerBlock = 1;
	float pixelsPerBlock = 16;
	float windowWidth = pixelsPerBlock * static_cast<int>(ImGui::GetContentRegionAvail().x / pixelsPerBlock);
	float pixelsPerByte = (pixelsPerBlock / bytesPerBlock);
	//   p        by         p*bl     p
	// ----    / ---   =>   ------ = ---
	//   bl       bl         bl*by    by
	
	const ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	float windowEnd = cursorScreenPos.x + windowWidth;

	auto drawAllocation = [=](ImVec2 cursor, uint32_t offset, uint32_t bytes, ImU32 color, ImU32 outlineColor, ImU32 hoverColor, bool used)
	{
		while (bytes > 0)
		{
			auto pixels = (uint32_t)pixelsPerByte * offset;
			auto row = pixels / (uint32_t)windowWidth;
			auto col = pixels % (uint32_t)windowWidth;
			ImVec2 start = cursor + ImVec2((float)col, row * pixelsPerBlock);

			auto bytesRoomLeft = static_cast<uint32_t>((windowEnd - start.x) / pixelsPerByte);
			auto bytesToDraw = std::min(bytes, bytesRoomLeft);
			ImVec2 end = start;
			end.x += pixelsPerByte * bytesToDraw;

			if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(start, end + ImVec2(0, pixelsPerBlock)))
			{
				color = hoverColor;
				ImGui::SetTooltip("Offset: %d, size: %d", offset, bytes);

				if (used && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					auto iter = std::find_if(allocations.begin(), allocations.end(), [offset](const Allocation& allocation) {
						return allocation.offset == offset;
					});

					assert(iter != allocations.end());
					auto allocationToFree = *iter;

					std::erase_if(allocations, [allocationToFree](const Allocation& alloc)
					{
						return alloc == allocationToFree;
					});
					allocator->free(allocationToFree);
				}
			}

			draw_list->AddRectFilled(start, end + ImVec2(0, pixelsPerBlock), outlineColor, 2.0f);
			draw_list->AddRectFilled(start + ImVec2(1, 1), end + ImVec2(-1, -1) + ImVec2(0, pixelsPerBlock), color, 2.0f);
			bytes -= bytesToDraw;
			offset += bytesToDraw;
		}
	};

	const auto allocatedColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
	const auto allocatedOutlineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
	const auto deallocatedColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
	const auto deallocatedOutlineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f));

	if (allocator)
	{
		auto drawAllocationChain = [=](uint32_t nodeIndex)
		{
			uint32_t i = nodeIndex;
			while (i != Allocator::Node::unused)
			{
				auto& node = allocator->m_nodes[i];

				auto color = node.used ? allocatedColor : deallocatedColor;
				auto outlineColor = node.used ? allocatedOutlineColor : deallocatedOutlineColor;
				auto hoverColor = node.used ? allocatedOutlineColor : deallocatedColor;

				drawAllocation(cursorScreenPos, node.dataOffset, node.dataSize, color, outlineColor, hoverColor, node.used);

				i = node.neighborPrev;
			}

			i = allocator->m_nodes[nodeIndex].neighborNext;
			while (i != Allocator::Node::unused)
			{
				auto& node = allocator->m_nodes[i];

				auto color = node.used ? allocatedColor : deallocatedColor;
				auto outlineColor = node.used ? allocatedOutlineColor : deallocatedOutlineColor;
				auto hoverColor = node.used ? allocatedOutlineColor : deallocatedColor;

				drawAllocation(cursorScreenPos, node.dataOffset, node.dataSize, color, outlineColor, hoverColor, node.used);

				i = node.neighborNext;
			}
		};

		for (int i = 0; i < 32; ++i)
		{
			if (allocator->m_usedBinsTop & (1 << i))
			{
				const auto leafBins = allocator->m_usedBins[i];
				for (int j = 0; j < 32; ++j)
				{
					if (leafBins & (1 << j))
					{
						uint32 binIndex = (i << TOP_BINS_INDEX_SHIFT) | j;
						uint32 nodeIndex = allocator->m_binIndices[binIndex];

						drawAllocationChain(nodeIndex);
					}
				}
			}
		}

		auto windowHeight = pixelsPerBlock * (pixelsPerByte * allocatorSize / windowWidth);

		ImGui::Dummy(ImVec2(windowWidth, windowHeight));

		ImGui::NewLine();
		drawAllocation(ImGui::GetCursorScreenPos(), 0, 1, deallocatedColor, deallocatedOutlineColor, deallocatedColor, false);
		ImGui::Dummy(ImVec2(pixelsPerBlock, pixelsPerBlock));
		ImGui::SameLine();
		ImGui::Text("Free block");
		drawAllocation(ImGui::GetCursorScreenPos(), 0, 1, allocatedColor, allocatedOutlineColor, allocatedColor, false);
		ImGui::Dummy(ImVec2(pixelsPerBlock, pixelsPerBlock));
		ImGui::SameLine();
		ImGui::Text("Allocated block");
	}

	ImGui::End();

	ImGui::Begin("Metadata");

	if (allocator)
	{
		ImGui::Text("Size: %d", allocator->m_size);
		ImGui::Text("Max allocs: %d", allocator->m_maxAllocs);
		ImGui::Text("Free storage: %d", allocator->m_freeStorage);
		for (uint32_t i = allocator->m_maxAllocs-1; i > allocator->m_freeOffset; --i)
		{
			const auto& nodeIndex = allocator->m_freeNodes[i];
			const auto& node = allocator->m_nodes[nodeIndex];
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			if (ImGui::TreeNode((void*)(intptr_t)i, "Node: %d", nodeIndex))
			{
				if (node.binListPrev != Allocator::Node::unused)
					ImGui::Text("Previous bin: %d", node.binListPrev);

				if (node.binListNext != Allocator::Node::unused)
					ImGui::Text("Next bin: %d", node.binListNext);

				ImGui::TreePop();
			}
		}
	}
	ImGui::End();

	ImGui::Begin("Nodes", 0, ImGuiWindowFlags_AlwaysHorizontalScrollbar);
	if (allocator)
	{
		ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
		ImU32 textColor = lineColor;
		ImU32 boxColorUsed = allocatedColor;
		ImU32 boxColorUnused = deallocatedColor;
		float lineThickness = 2.0f;
		float margin = 4.0f;
		float rounding = 4.0f;
		ImVec2 size(ImGui::CalcTextSize(std::format("O: {}", allocatorSize).c_str()).x + 2 * margin, 48);
		for (int i = 0; i < 32; ++i)
		{
			if (allocator->m_usedBinsTop & (1 << i))
			{
				const auto leafBins = allocator->m_usedBins[i];
				for (int j = 0; j < 32; ++j)
				{
					if (leafBins & (1 << j))
					{
						uint32 binIndex = (i << TOP_BINS_INDEX_SHIFT) | j;
						uint32 binSize = OffsetAllocator::SmallFloat::floatToUint(binIndex);
						
						ImVec2 pos = ImGui::GetCursorScreenPos();
						ImGui::GetWindowDrawList()->AddText(pos, textColor, std::format("{} ({})", binSize, binIndex).c_str());
						pos.x += 100;
						uint32 nodeIndex = allocator->m_binIndices[binIndex];
						ImVec2 contentSize = ImVec2(100, 0) + size;
						while (nodeIndex != Allocator::Node::unused)
						{
							const auto& node = allocator->m_nodes[nodeIndex];
							auto color = node.used ? boxColorUsed : boxColorUnused;
							DrawAllocatorNode(pos, nodeIndex, node.dataOffset, node.dataSize, lineColor, color, textColor, size, rounding, lineThickness, margin);
							nodeIndex = node.neighborPrev;
							pos.x += size.x + 10;
							contentSize.x += size.x + 10;
						}
						ImGui::Dummy(contentSize);
					}
				}
			}
		}
	}
	

	ImGui::Dummy(ImGui::GetContentRegionAvail());
	ImGui::End();
}

int main(int, char**)
{
	WNDCLASSEXW wc = {sizeof(wc), CS_OWNDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"ImGui Example", NULL};
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"OffsetAllocator Explorer", WS_OVERLAPPEDWINDOW, 100, 100, 1400, 800, NULL, NULL, wc.hInstance, NULL);

	if (!CreateDeviceWGL(hwnd, &g_MainWindow))
	{
		CleanupDeviceWGL(hwnd, &g_MainWindow);
		::DestroyWindow(hwnd);
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}
	wglMakeCurrent(g_MainWindow.hDC, g_hRC);

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui_ImplWin32_InitForOpenGL(hwnd);
	ImGui_ImplOpenGL3_Init();

	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	bool done = false;
	while (!done)
	{
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ShowAllocatorExplorer();

		ImGui::Render();
		glViewport(0, 0, g_Width, g_Height);
		glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		::SwapBuffers(g_MainWindow.hDC);
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceWGL(hwnd, &g_MainWindow);
	wglDeleteContext(g_hRC);
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}

bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data)
{
	HDC hDc = ::GetDC(hWnd);
	PIXELFORMATDESCRIPTOR pfd = {0};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;

	const int pf = ::ChoosePixelFormat(hDc, &pfd);
	if (pf == 0)
		return false;
	if (::SetPixelFormat(hDc, pf, &pfd) == FALSE)
		return false;
	::ReleaseDC(hWnd, hDc);

	data->hDC = ::GetDC(hWnd);
	if (!g_hRC)
		g_hRC = wglCreateContext(data->hDC);
	return true;
}

void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data)
{
	wglMakeCurrent(NULL, NULL);
	::ReleaseDC(hWnd, data->hDC);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED)
			{
				g_Width = LOWORD(lParam);
				g_Height = HIWORD(lParam);
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
				return 0;
			break;
		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE)
			{
				DestroyWindow(hWnd);
			}
			break;
		case WM_DESTROY:
			::PostQuitMessage(0);
			return 0;
	}
	return ::DefWindowProc(hWnd, msg, wParam, lParam);
}
