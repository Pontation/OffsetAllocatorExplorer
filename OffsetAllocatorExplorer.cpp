#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "OffsetAllocator/offsetAllocator.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/GL.h>
#include <tchar.h>

#include <vector>
#include <memory>

bool operator==(const OffsetAllocator::Allocation& left, const OffsetAllocator::Allocation& right)
{
	return left.offset == right.offset && left.metadata == right.metadata;
}

using namespace OffsetAllocator;

struct WGL_WindowData { HDC hDC; };

static HGLRC g_hRC;
static WGL_WindowData g_MainWindow;
static int g_Width;
static int g_Height;
std::unique_ptr<Allocator> allocator;
std::vector<OffsetAllocator::Allocation> allocations;

bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data);
void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ShowAllocatorExplorer()
{
	ImGui::Begin("Offset Allocator Explorer");

	if (allocator)
	{
		auto topLevelReport = allocator->storageReport();

		ImGui::Text("Total free space: %d", topLevelReport.totalFreeSpace);
		ImGui::Text("Largest free region: %d", topLevelReport.largestFreeRegion);
		ImGui::NewLine();

		auto fullReport = allocator->storageReportFull();
		for (const auto& region : fullReport.freeRegions)
		{
			if (region.count > 0)
			{
				ImGui::Text("Count: %d, size: %d", region.count, region.size);
			}
		}
		ImGui::NewLine();

		ImGui::Text("Allocations:");
		OffsetAllocator::Allocation allocationToFree;
		int id = 0;
		for (OffsetAllocator::Allocation allocation : allocations)
		{
			ImGui::PushID(id++);
			ImGui::Text("Offset: %d, size: %d", allocation.offset, allocator->allocationSize(allocation));
			ImGui::SameLine();
			if (ImGui::Button("Free"))
			{
				allocationToFree = allocation;
			}
			ImGui::PopID();
		}

		if (allocationToFree.offset != Allocation::NO_SPACE)
		{
			std::erase_if(allocations, [allocationToFree](const OffsetAllocator::Allocation& alloc) {
				return alloc == allocationToFree;
			});
			allocator->free(allocationToFree);
		}

		ImGui::NewLine();

		static int allocationSize = 1;
		ImGui::InputInt("Size", &allocationSize);
		ImGui::SameLine();
		if (ImGui::Button("Allocate"))
		{
			auto allocation = allocator->allocate(allocationSize);
			if (allocation.offset != Allocation::NO_SPACE)
			{
				allocations.emplace_back(allocation);
			}
		}
		
		ImGui::NewLine();
		if (ImGui::Button("New Allocator"))
		{
			allocator.reset();
		}
	}
	else
	{
		static int allocatorSize = 1024;
		ImGui::InputInt("Size", &allocatorSize);

		static int maxAllocs = 128 * 1024;
		ImGui::InputInt("Max Allocations", &maxAllocs);

		if (ImGui::Button("Create Allocator"))
		{
			allocator = std::make_unique<Allocator>(allocatorSize, maxAllocs);
		}
	}

	ImGui::End();
}

int main(int, char**)
{
	//ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = {sizeof(wc), CS_OWNDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"ImGui Example", NULL};
	::RegisterClassExW(&wc);
	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"OffsetAllocator Explorer", WS_OVERLAPPEDWINDOW, 100, 100, 900, 800, NULL, NULL, wc.hInstance, NULL);

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
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // Enable Keyboard Controls

	ImGui::StyleColorsDark();
	//ImGui::StyleColorsClassic();

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
