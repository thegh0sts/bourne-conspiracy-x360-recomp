// From Glue2GPU (https://github.com/zer0ducksgiven/Glue2GPU, MIT, licenses/Glue2GPU-LICENSE.txt).
#pragma once

namespace glue2gpu
{
// Logs every other thread's host stack (Host::logThreadStacks), and appends it to
// regpu_stacks.txt in the working directory.
void LogThreadStacks(const char* why);
} // namespace glue2gpu
