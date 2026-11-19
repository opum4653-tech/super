#pragma once
// ============================================================================
// CENTRALIZED OFFSETS ALIAS FORWARDER
// ============================================================================
// Project: crack_master
//
// ARCHITECTURAL MANDATE:
// Do not define new offsets here. All game field offsets, class offsets, and
// structure layouts are centralized in: src/offsets/offsets.hpp.
//
// When updating for game patches (OB56, OB57, etc.), update only src/offsets/offsets.hpp.
// This file exists to satisfy existing includes across the legacy code tree.
// ============================================================================

#include "../offsets/offsets.hpp"

