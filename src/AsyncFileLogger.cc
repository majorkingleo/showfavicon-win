#include "AsyncFileLogger.h"
#include <stderr_exception.h>
#include <utf8_util.h>
#include <filesystem>
#include <format>

using namespace AsyncOut;
using namespace Tools;

FileLogger::FileLogger( const std::string & filename_ )
: m_filename(filename_)
{
    m_file.open( m_filename, std::ios::out | std::ios::app );

    if( !m_file ) {
        throw STDERR_EXCEPTION( Tools::format( "cannot open log file '%s'", m_filename ) );
    }


	if (auto env = getenv("TZ")) {
		tz = std::chrono::locate_zone(env);
	}

	if (!tz) {
		tz = std::chrono::current_zone();
	}	
}

void FileLogger::log()
{
    auto lock = std::lock_guard(m_file_mutex);

	auto all_messages = popAll();

	for( const auto & m : all_messages ) {

		const auto local_when = tz->to_local(std::chrono::utc_clock::to_sys( m.when ));
		const auto local_ms_when = std::chrono::time_point_cast<std::chrono::milliseconds>(local_when);
		
		std::string timestamp = std::format( "{0:%Y}-{0:%m}-{0:%d} {0:%H}:{0:%M}:{0:%S}", local_ms_when );
		m_file << "[" << timestamp << "] ";

		std::string file_name = std::filesystem::path(m.file).filename().string();
		m_file << file_name;
		m_file << ':' << m.line
 			   << " ";

		std::string message;

		if( const auto str_ptr (std::get_if<std::string>(&m.message)); str_ptr) {
			message = *str_ptr;
		} else {
			message = Utf8Util::wStringToUtf8(std::get<std::wstring>(m.message));
		}

		m_file << message << '\n';
	}
}
