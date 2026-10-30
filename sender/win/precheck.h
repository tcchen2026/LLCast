#pragma once
// Checks LLCast runs before streaming, each explained in plain words. They
// read state only: no capture, no settings changes. The receiver check sends
// a few PINGs, which a receiver answers whatever it is doing.

#include "pipeline.h"

#include <cstdint>
#include <string>

namespace lls {

struct Problem {
    std::wstring headline; // what is wrong
    std::wstring advice;   // what to do about it
    std::wstring detail;   // technical detail, may be empty
};

// Parses the typed address and port. Host names are resolved.
bool check_address(const std::wstring &host, const std::wstring &port,
                   std::string &host_out, uint16_t &port_out, Problem &problem);
// The process must be on the unlocked, local console desktop.
bool check_desktop(Problem &problem);
// The primary display must use a hardware adapter and be unrotated SDR.
bool check_display(Problem &problem);
// Probe the hardware encoder on the primary display GPU; no screen capture.
bool check_encoder(uint32_t codecs, const EncoderConfig &config, Problem &problem,
                   EncoderBackend backend = EncoderBackend::Auto);
// A receiver must answer PING at host:port. rtt_us is its reply time.
bool check_receiver(const std::string &host, uint16_t port, uint32_t &rtt_us, uint32_t &codecs, Problem &problem);
// Explains a pipeline failure; the caller adds the log as detail.
Problem explain(PipelineStep step);

} // namespace lls
