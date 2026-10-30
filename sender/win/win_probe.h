#pragma once
namespace lls {
// Prints all adapters/outputs and probes the chosen display's pipeline.
// No capture frames, files, network traffic or persistent settings changes.
bool print_windows_info(int display_index);
}
