/*
 * sc4-region-sync
 *
 * WriterIdentity.h - who wrote a snapshot, and whether that process still
 * runs. Windows-only, no game headers.
 */

#pragma once
#include "NeighborSnapshot.h"

namespace regionsync::writer
{
	enum class Liveness { Alive, Dead, Unknown };

	// This process: computer name, pid, creation time.
	WriterIdentity Current();

	// Unknown when the identity is incomplete or from another host. Dead when
	// the pid is gone, has exited, or now belongs to a process with a
	// different creation time (pid reuse).
	Liveness IsAlive(const WriterIdentity& id);
}
