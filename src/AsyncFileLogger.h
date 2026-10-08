#pragma once

#include "AsyncOutDebug.h"
#include <fstream>
#include <chrono>

namespace AsyncOut {

class FileLogger : public AsyncOut::Logger
{
    std::string                             m_filename;
    std::ofstream                           m_file;
    std::mutex                              m_file_mutex;
    const std::chrono::time_zone*           tz = nullptr;

public:
    FileLogger( const std::string & filename_ );

    void log() override;

    void flush() {
        auto lock = std::lock_guard(m_file_mutex);
        m_file.flush();
    }
};

} // namespace AsyncOut
