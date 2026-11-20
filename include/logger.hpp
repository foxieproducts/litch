#pragma once

#include "foxie_embedded/logger.hpp"

namespace logger {

using fxe::ClearBufferedLines;
using fxe::Debug;
using fxe::DumpBufferedLines;
using fxe::Error;
using fxe::GetMinLevel;
using fxe::Info;
using fxe::Level;
using fxe::OutputLineCallback;
using fxe::RawLine;
using fxe::SetMinLevel;
using fxe::SetOutputLineCallback;
using fxe::Trace;
using fxe::Warn;

} // namespace logger
