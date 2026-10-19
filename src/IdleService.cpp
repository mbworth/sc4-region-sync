/*
 * sc4-region-sync
 *
 * IdleService.cpp - see IdleService.h.
 */

#include "IdleService.h"
#include "Logger.h"
#include <utility>

namespace regionsync
{
	IdleService::IdleService(uint32_t serviceID) : cRZBaseSystemService(serviceID, 1000000)
	{
	}

	void IdleService::SetCallback(std::function<void()> fn)
	{
		callback = std::move(fn);
	}

	bool IdleService::Init()
	{
		SetServiceRunning(true);
		return true;
	}

	bool IdleService::Shutdown()
	{
		SetServiceRunning(false);
		return true;
	}

	bool IdleService::OnIdle(uint32_t)
	{
		try
		{
			if (callback)
			{
				callback();
			}
		}
		catch (...)
		{
			if (!threwLogged)
			{
				threwLogged = true;
				Logger::GetInstance().WriteLine(LogLevel::Error, "IdleService: callback threw (logged once).");
			}
		}
		return true;
	}
}
