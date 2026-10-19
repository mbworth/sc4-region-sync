/*
 * sc4-region-sync
 *
 * IdleService.h - system service that calls one callback from OnIdle on the game thread (Q17).
 */

#pragma once
#include "cRZBaseSystemService.h"
#include <functional>

namespace regionsync
{
	class IdleService final : public cRZBaseSystemService
	{
	public:
		explicit IdleService(uint32_t serviceID);

		void SetCallback(std::function<void()> fn);

		bool Init() override;
		bool Shutdown() override;
		bool OnIdle(uint32_t) override;

	private:
		std::function<void()> callback;
		bool threwLogged = false;
	};
}
