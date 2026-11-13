#pragma once

#include <string>
#include <ctime>

namespace lic {

    bool is_valid();
    void init();
    bool set_key(const std::string& key);
    time_t expiry_date();
    void open_update_page();

}
