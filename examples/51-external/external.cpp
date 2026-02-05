/*
 * Copyright 2026 Daniele Bartolini. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include <bgfx/platform.h>
#include <bgfx_utils.h>
#include <bx/process.h>
#include <bx/timer.h>
#include <entry/entry.h>
#include <entry/input.h>
#include "imgui/imgui.h"

#if BX_PLATFORM_LINUX
#	include <unistd.h>
#	include <stdlib.h>
#	include <sys/socket.h>
#	include <sys/un.h>     // Unix-domain socket.
#elif BX_PLATFORM_WINDOWS
#	include <windows.h>
#endif

void cmdCreateImporterProcess(const void* _userData);

void sendOrReceiveTextureInfo(bgfx::ExternalTextureInfo& info, bool receive = false)
{
	BX_TRACE("Waiting for %s process...", (receive ? "Exporter" : "Importer") );

#if BX_PLATFORM_LINUX
	const char *serverPath = "./server";
	const char *clientPath = "./client";
	const char *path = receive ? clientPath : serverPath;
	int dmaBufFd = 0;

	int sockFd = socket(AF_UNIX, SOCK_DGRAM, 0);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr) );
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);

	unlink(path);

	if (bind(sockFd, (struct sockaddr *)&addr, sizeof(addr) ) < 0)
	{
		exit(-1);
	}

	if (receive == false) // Server.
	{
		dmaBufFd = (int)(uintptr_t)info.handle;
		if (0 >= dmaBufFd)
		{
			close(sockFd);
			exit(EXIT_FAILURE);
		}

		// Wait for client.
		memset(&addr, 0, sizeof(addr) );
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, clientPath);
		while (0 != connect(sockFd, (struct sockaddr *)&addr, sizeof(addr) ) )
		{
		}

		// Send texture info to client.
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg) );
		char buf[CMSG_SPACE(sizeof(dmaBufFd) )];
		memset(buf, '\0', sizeof(buf) );

		struct iovec io = {.iov_base = &info, .iov_len = sizeof(info)};

		msg.msg_iov = &io;
		msg.msg_iovlen = 1;
		msg.msg_control = buf;
		msg.msg_controllen = sizeof(buf);

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(dmaBufFd) );

		memmove(CMSG_DATA(cmsg), &dmaBufFd, sizeof(dmaBufFd) );

		msg.msg_controllen = CMSG_SPACE(sizeof(dmaBufFd) );

		if (sendmsg(sockFd, &msg, 0) < 0)
		{
			exit(-1);
		}

		close(sockFd);
		close(dmaBufFd);
	}
	else // Client.
	{
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg) );

		struct iovec io = {.iov_base = &info, .iov_len = sizeof(info)};
		msg.msg_iov = &io;
		msg.msg_iovlen = 1;

		char c_buffer[256];
		msg.msg_control = c_buffer;
		msg.msg_controllen = sizeof(c_buffer);

		if (recvmsg(sockFd, &msg, 0) < 0)
		{
			exit(-1);
		}

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

		memmove(&dmaBufFd, CMSG_DATA(cmsg), 4);

		info.handle = (void *)(uintptr_t)dmaBufFd;
		close(sockFd);
	}
#else
	static const char* PIPE_NAME = R"(\\.\pipe\data)";

	#pragma pack(push,1)
	struct ExternalTextureInfoPacket
	{
		bgfx::ExternalTextureInfo info;
		uint32_t pid;
	};
	#pragma pack(pop)

	if (!receive)
	{
		HANDLE hPipe = CreateNamedPipeA(
			  PIPE_NAME
			, PIPE_ACCESS_OUTBOUND
			, PIPE_TYPE_BYTE | PIPE_WAIT
			, 1
			, sizeof(ExternalTextureInfoPacket)
			, sizeof(ExternalTextureInfoPacket)
			, 0
			, NULL
			);

		if (hPipe == INVALID_HANDLE_VALUE)
		{
			BX_TRACE("CreateNamedPipe failed (err: 0x%08x).", GetLastError());
			return;
		}

		// Wait for client to connect.
		BOOL ok = ConnectNamedPipe(hPipe, NULL);
		if (!ok)
		{
			DWORD err = GetLastError();
			if (err != ERROR_PIPE_CONNECTED)
			{
				BX_TRACE("ConnectNamedPipe failed (err: 0x%08x).", (unsigned long)err);
				CloseHandle(hPipe);
				return;
			}
		}

		// Fill wire structure.
		ExternalTextureInfoPacket w;
		w.info = info;
		w.pid  = (uint32_t)GetCurrentProcessId();

		DWORD bytesWritten = 0;
		if (!WriteFile(hPipe, &w, sizeof(w), &bytesWritten, NULL) || sizeof(w) != bytesWritten)
		{
			BX_TRACE("WriteFile to pipe failed (err: 0x%08x) (written: %u).", GetLastError(), bytesWritten);
		}

		FlushFileBuffers(hPipe);
		DisconnectNamedPipe(hPipe);
		CloseHandle(hPipe);
	}
	else
	{
		if (!WaitNamedPipeA(PIPE_NAME, NMPWAIT_WAIT_FOREVER))
		{
			BX_TRACE("WaitNamedPipe failed (err: 0x%08x).", GetLastError());
			return;
		}

		HANDLE hPipe = CreateFileA(
			  PIPE_NAME
			, GENERIC_READ
			, 0
			, NULL
			, OPEN_EXISTING
			, 0
			, NULL
			);

		if (INVALID_HANDLE_VALUE == hPipe)
		{
			BX_TRACE("CreateFile (open pipe) failed (err: 0x%08x).", GetLastError());
			return;
		}

		ExternalTextureInfoPacket w;
		DWORD bytesRead = 0;
		if (!ReadFile(hPipe, &w, sizeof(w), &bytesRead, NULL) || bytesRead != sizeof(w))
		{
			BX_TRACE("ReadFile from pipe failed (err: 0x%08x) (read: %u).", GetLastError(), bytesRead);
			CloseHandle(hPipe);
			return;
		}

		CloseHandle(hPipe);

		info = w.info;

		// Duplicate handle.
		if (w.info.handle != NULL && w.pid != 0)
		{
			DWORD sourcePid = (DWORD)w.pid;
			HANDLE hSourceProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid);
			if (hSourceProc == NULL)
			{
				BX_TRACE("OpenProcess(PROCESS_DUP_HANDLE) failed for pid %u (err: 0x%08x).", sourcePid, GetLastError());
				return;
			}

			HANDLE dupHandle = NULL;
			HANDLE hSourceHandle = (HANDLE)(uintptr_t)w.info.handle;
			BOOL dupOk = DuplicateHandle(
				  hSourceProc
				, hSourceHandle
				, GetCurrentProcess()
				, &dupHandle
				, 0
				, FALSE
				, DUPLICATE_SAME_ACCESS
				);

			CloseHandle(hSourceProc);

			if (!dupOk)
			{
				BX_TRACE("DuplicateHandle failed (err: 0x%08x).", GetLastError());
				return;
			}

			info.handle = (void*)dupHandle;
		}
	}
#endif
}

void sendTextureInfo(bgfx::ExternalTextureInfo& info)
{
	sendOrReceiveTextureInfo(info);
}

void receiveTextureInfo(bgfx::ExternalTextureInfo& info)
{
	sendOrReceiveTextureInfo(info, true);
}

struct PosTexCoord0Vertex
{
	float m_x;
	float m_y;
	float m_z;
	float m_u;
	float m_v;

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout PosTexCoord0Vertex::ms_layout;

void renderScreenSpaceQuad(uint8_t _view, bgfx::ProgramHandle _program, float _x, float _y, float _width, float _height)
{
	bgfx::TransientVertexBuffer tvb;
	bgfx::TransientIndexBuffer tib;

	if (bgfx::allocTransientBuffers(&tvb, PosTexCoord0Vertex::ms_layout, 4, &tib, 6) )
	{
		PosTexCoord0Vertex* vertex = (PosTexCoord0Vertex*)tvb.data;

		float zz = 0.0f;

		const float minx = _x;
		const float maxx = _x + _width;
		const float miny = _y;
		const float maxy = _y + _height;

		float minu = -1.0f;
		float minv = -1.0f;
		float maxu =  1.0f;
		float maxv =  1.0f;

		vertex[0].m_x = minx;
		vertex[0].m_y = miny;
		vertex[0].m_z = zz;
		vertex[0].m_u = minu;
		vertex[0].m_v = minv;

		vertex[1].m_x = maxx;
		vertex[1].m_y = miny;
		vertex[1].m_z = zz;
		vertex[1].m_u = maxu;
		vertex[1].m_v = minv;

		vertex[2].m_x = maxx;
		vertex[2].m_y = maxy;
		vertex[2].m_z = zz;
		vertex[2].m_u = maxu;
		vertex[2].m_v = maxv;

		vertex[3].m_x = minx;
		vertex[3].m_y = maxy;
		vertex[3].m_z = zz;
		vertex[3].m_u = minu;
		vertex[3].m_v = maxv;

		uint16_t* indices = (uint16_t*)tib.data;

		indices[0] = 0;
		indices[1] = 2;
		indices[2] = 1;
		indices[3] = 0;
		indices[4] = 3;
		indices[5] = 2;

		bgfx::setState(BGFX_STATE_DEFAULT);
		bgfx::setIndexBuffer(&tib);
		bgfx::setVertexBuffer(0, &tvb);
		bgfx::submit(_view, _program);
	}
}

constexpr uint16_t kWidth  = 512;
constexpr uint16_t kHeight = 512;

class ExampleExternal : public entry::AppI
{
public:
	ExampleExternal(const char* _name, const char* _description, const char* _url)
		: entry::AppI(_name, _description, _url)
		, m_importerProcessOpen(false)
	{
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);

		m_width  = _width;
		m_height = _height;
		m_debug  = BGFX_DEBUG_TEXT;
		m_reset  = BGFX_RESET_VSYNC;

		bgfx::Init init;
		init.type     = args.m_type;
		init.vendorId = args.m_pciId;
		init.platformData.nwh  = entry::getNativeWindowHandle(entry::kDefaultWindowHandle);
		init.platformData.ndt  = entry::getNativeDisplayHandle();
		init.platformData.type = entry::getNativeWindowHandleType();
		init.resolution.width  = m_width;
		init.resolution.height = m_height;
		init.resolution.reset  = m_reset;
		bgfx::init(init);

		m_cmdLine = BX_NEW(entry::getAllocator(), bx::CommandLine)(_argc, (const char**)_argv);
		m_exporter = m_cmdLine->hasArg("export") || !m_cmdLine->hasArg("import");

		m_bindings = (InputBinding*)bx::alloc(entry::getAllocator(), sizeof(InputBinding)*2);
		m_bindings[0].set(entry::Key::KeyC, entry::Modifier::None, 1, cmdCreateImporterProcess, this);
		m_bindings[1].end();

		inputAddBindings("51-external", m_bindings);

		m_caps = bgfx::getCaps();
		m_externalTextureSupported = 0 != (m_caps->supported & BGFX_CAPS_EXTERNAL_MEMORY);

		bgfx::setDebug(m_debug);

		if (m_externalTextureSupported)
		{
			PosTexCoord0Vertex::init();
			s_color = bgfx::createUniform("s_color", bgfx::UniformType::Sampler);
			m_program = loadProgram("vs_blit", "fs_blit");

			m_texture = bgfx::createTexture2D(
				  kWidth
				, kHeight
				, false
				, 1
				, bgfx::TextureFormat::RGBA8
				, BGFX_TEXTURE_RT | (m_exporter ? BGFX_TEXTURE_EXPORT : BGFX_TEXTURE_IMPORT)
				);

			if (m_exporter)
			{
				bgfx::exportTexture(m_texture, m_externalTextureInfo);

				bgfx::frame();
				bgfx::frame();
			}
			else
			{
				receiveTextureInfo(m_externalTextureInfo);

				bgfx::importTexture(m_texture, m_externalTextureInfo);
			}

			BX_TRACE("ExternalTextureInfo:");
			BX_TRACE("\tstride   %u", m_externalTextureInfo.stride);
			BX_TRACE("\toffset   %u", m_externalTextureInfo.offset);
			BX_TRACE("\tsize     %u", m_externalTextureInfo.size);
			BX_TRACE("\tfourcc   %.8x", m_externalTextureInfo.fourcc);
			BX_TRACE("\tmodifier %.16llx", m_externalTextureInfo.modifier);
			BX_TRACE("\thandle   %p", m_externalTextureInfo.handle);

			const bgfx::TextureHandle attachment[] = { m_texture };
			m_fbh = bgfx::createFrameBuffer(BX_COUNTOF(attachment), attachment);
		}

		imguiCreate();
	}

	virtual int shutdown() override
	{
		imguiDestroy();

		inputRemoveBindings("51-external");
		bx::free(entry::getAllocator(), m_bindings);

		bx::free(entry::getAllocator(), m_cmdLine);

		if (m_externalTextureSupported)
		{
			bgfx::destroy(m_program);
			bgfx::destroy(s_color);
			bgfx::destroy(m_texture);
			bgfx::destroy(m_fbh);

			if (m_exporter)
			{
#if BX_PLATFORM_LINUX
				close((int)(uintptr_t)m_externalTextureInfo.handle);
#elif BX_PLATFORM_WINDOWS
				CloseHandle((HANDLE)(uintptr_t)m_externalTextureInfo.handle);
#endif
			}
		}

		if (m_exporter)
		{
			if (m_importerProcessOpen)
			{
				m_importerProcess.close();
			}
		}

		bgfx::shutdown();

		return 0;
	}

	bool update() override
	{
		if (!entry::processWindowEvents(m_state, m_debug, m_reset) )
		{
			entry::MouseState mouseState = m_state.m_mouse;

			imguiBeginFrame(mouseState.m_mx
				,  mouseState.m_my
				, (mouseState.m_buttons[entry::MouseButton::Left  ] ? IMGUI_MBUT_LEFT   : 0)
				| (mouseState.m_buttons[entry::MouseButton::Right ] ? IMGUI_MBUT_RIGHT  : 0)
				| (mouseState.m_buttons[entry::MouseButton::Middle] ? IMGUI_MBUT_MIDDLE : 0)
				,  mouseState.m_mz
				, uint16_t(m_width)
				, uint16_t(m_height)
				);

			showExampleDialog(this);

			imguiEndFrame();

			bgfx::setViewRect(0, 0, 0, kWidth, kHeight );
			bgfx::setViewRect(1, 0, 0, uint16_t(m_width), uint16_t(m_height) );

			// This dummy draw call is here to make sure that view 1 is cleared
			// if no other draw calls are submitted to view 1.
			bgfx::setViewClear(1
				, BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH
				, 0x303030ff
				, 1.0f
				, 0
				);
			bgfx::touch(1);

			constexpr uint32_t colors[] = { 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xff00ffff, 0xffff00ff, 0x00ffffff };

			bgfx::dbgTextClear();
			bgfx::dbgTextPrintf(1, 1, 0x2f, m_exporter ? "Exporter Process" : "Importer Process");

			if (!m_externalTextureSupported)
			{
				bgfx::dbgTextPrintf(1, 2, 0x4f, "External Texture not supported!");
			}
			else
			{
				if (m_exporter)
				{
					bgfx::dbgTextPrintf(1, 2, 0x2f, "Press 'c' to import the texture in a different process.");

					// Clear external texture.
					bgfx::setViewFrameBuffer(0, m_fbh);
					bgfx::setViewClear(0
						, BGFX_CLEAR_COLOR
						, colors[bx::toSeconds<uint32_t>(bx::getTicksSinceStartup() ) % BX_COUNTOF(colors)]
						, 1.0f
						, 0
						);
					bgfx::touch(0);
				}

				// Draw external texture to backbuffer.
				float ortho[16];
				bx::mtxOrtho(ortho, 0.0f, (float)m_width, (float)m_height, 0.0f, 0.0f, 100.0f, 0.0, m_caps->homogeneousDepth);

				bgfx::setViewTransform(1, NULL, ortho);

				bgfx::setTexture(0, s_color, bgfx::getTexture(m_fbh) );
				renderScreenSpaceQuad(
					  1
					, m_program
					, 0.5f * (m_width - kWidth)
					, 0.5f * (m_height - kHeight)
					, kWidth
					, kHeight
					);
			}

			// Advance to next frame. Rendering thread will be kicked to
			// process submitted rendering primitives.
			bgfx::frame();

			return true;
		}

		return false;
	}

	void createImporterProcess()
	{
		if (m_importerProcessOpen)
		{
			m_importerProcess.close();
			m_importerProcessOpen = false;
		}

		m_importerProcessOpen = m_importerProcess.open(
			  bx::FilePath(m_cmdLine->get(0))
			, "--vk --import"
			, &m_errorIgnore
			);

		sendTextureInfo(m_externalTextureInfo);
	}

	bx::CommandLine* m_cmdLine;
	entry::WindowState m_state;

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_debug;
	uint32_t m_reset;
	bool m_exporter;
	bool m_externalTextureSupported;
	const bgfx::Caps* m_caps;
	bgfx::ExternalTextureInfo m_externalTextureInfo;
	bgfx::TextureHandle m_texture;
	bgfx::FrameBufferHandle m_fbh;
	bgfx::ProgramHandle m_program;
	bgfx::UniformHandle s_color;

	bx::ErrorIgnore m_errorIgnore;
	bx::ProcessWriter m_importerProcess;
	bool m_importerProcessOpen;

	InputBinding* m_bindings;
};

ENTRY_IMPLEMENT_MAIN(
	  ExampleExternal
	, "51-external"
	, "Share texture across processes."
	, "https://bkaradzic.github.io/bgfx/examples.html#external"
	);

void cmdCreateImporterProcess(const void* _userData)
{
	( (ExampleExternal*)_userData)->createImporterProcess();
}
