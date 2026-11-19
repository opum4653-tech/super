#pragma once
// ============================================================================
// RAM-ONLY LICENSE AUTHENTICATION SUBSYSTEM INTERFACE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Controls security licensing and API key gating:
// - RAM-Only Security Model: Keys are never persisted to disk or flash storage.
// - Hardware Binding: Matches device HWID (Android ID / ro.serialno).
// - Verification Backend: Secure HTTPS handshake via libcurl with in-memory CA bundle.
// ============================================================================

#include <string>
#include <ctime>

namespace lic {

    bool is_valid();                        // Returns true if core engine is verified and authorized
    void init();                            // Initializes license state in RAM
    bool set_key(const std::string& key);   // Validates access key with remote licensing backend
    time_t expiry_date();                   // Returns Unix timestamp of subscription expiry

} // namespace lic

