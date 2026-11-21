#pragma once


#include <atomic>

class FrameDiagnostics {
public:
	enum class Stage : uint64_t { Idle, Copy, Flush, Map, Unmap, BeginFrameBuffer, CommitFrame };

	class Operation {
	public:
		Operation(FrameDiagnostics& owner, Stage stage) : owner(owner)
		{
			owner.active.store((GetTickCount64() << 8) | static_cast<uint64_t>(stage));
		}
		~Operation() { owner.active.store(0); }
		Operation(const Operation&) = delete;
		Operation& operator=(const Operation&) = delete;
	private:
		FrameDiagnostics& owner;
	};

	~FrameDiagnostics() { Stop(); }

	void Start()
	{
		completed.store(GetTickCount64());
		stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (stopEvent)
			worker = CreateThread(nullptr, 0, Run, this, 0, nullptr);
		if (worker) {
			VLOG("frame-watchdog enabled");
		} else {
			if (stopEvent) CloseHandle(stopEvent);
			stopEvent = nullptr;
			VLOG("frame-watchdog could not start");
		}
	}

	void Stop()
	{
		if (!worker)
			return;
		SetEvent(stopEvent);
		WaitForSingleObject(worker, INFINITE);
		CloseHandle(worker);
		CloseHandle(stopEvent);
		worker = stopEvent = nullptr;
	}

	void FrameCompleted() { completed.store(GetTickCount64()); }

private:
	static const char* Name(Stage stage)
	{
		switch (stage) {
		case Stage::Copy: return "D3D11 CopySubresourceRegion";
		case Stage::Flush: return "D3D11 Flush";
		case Stage::Map: return "D3D11 Map";
		case Stage::Unmap: return "D3D11 Unmap";
		case Stage::BeginFrameBuffer: return "bridge BeginFrame";
		case Stage::CommitFrame: return "bridge CommitFrame";
		default: return "none observed";
		}
	}

	static DWORD WINAPI Run(void* context)
	{
		static_cast<FrameDiagnostics*>(context)->Monitor();
		return 0;
	}

	void Monitor()
	{
		bool reported = false;
		uint64_t reportedOperation = 0;
		while (WaitForSingleObject(stopEvent, 250) == WAIT_TIMEOUT) {
			const uint64_t last = completed.load();
			const uint64_t operation = active.load();
			const uint64_t now = GetTickCount64();
			if (now - last >= 3000) {
				const bool blocked = operation && now - (operation >> 8) >= 1000;
				if (!reported || (blocked && operation != reportedOperation)) {
					VLOG("frame-watchdog: no xrEndFrame completion for %llu ms, active operation: %s (%llu ms)",
					    (unsigned long long)(now - last),
					    Name(blocked ? static_cast<Stage>(operation & 255) : Stage::Idle),
					    (unsigned long long)(blocked ? now - (operation >> 8) : 0));
					reported = true;
					reportedOperation = blocked ? operation : 0;
				}
			} else if (reported) {
				VLOG("frame-watchdog: xrEndFrame progress resumed");
				reported = false;
				reportedOperation = 0;
			}
		}
	}

	std::atomic<uint64_t> active{0};
	std::atomic<uint64_t> completed{0};
	HANDLE stopEvent = nullptr;
	HANDLE worker = nullptr;
};
