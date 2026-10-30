#pragma once
#include <windows.h>
extern "C" {
#include "lls_profile.h"
}

namespace llcast {
void receive_window(HWND parent, bool tearing, const LlsProfileConfig &profile);
}
