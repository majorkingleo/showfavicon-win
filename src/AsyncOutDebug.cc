/**
 * class for printing debugging messages async to stdout
 * @author Copyright (c) 2023 Martin Oberzalek
 */
#include "AsyncOutDebug.h"
#include <filesystem>
#include <iostream>

using namespace Tools;
using namespace AsyncOut;

AsyncOut::Debug::Debug( ColoredOutput::Color color )
: OutDebug(color)
{

}

#ifdef __cpp_lib_string_view
void AsyncOut::Debug::add( const char *file, unsigned line, const char *function, const std::string_view & s )
{
	distribute( Data{file,line,function, std::string(s), color, prefix} );
}

void AsyncOut::Debug::add( const char *file, unsigned line, const char *function, const std::wstring_view & s )
{
	distribute( Data{file,line,function, std::wstring(s), color, prefix} );
}
#else
void AsyncOut::Debug::add( const char *file, unsigned line, const char *function, const std::string & s )
{
	distribute( Data{file,line,function, s, color, prefix} );
}

void AsyncOut::Debug::add( const char *file, unsigned line, const char *function, const std::wstring & s )
{
	distribute( Data{file,line,function, s, color, prefix} );
}
#endif

void AsyncOut::Logger::deliver( const value_type & msg )
{
	std::lock_guard<std::mutex> ml(m_messages);
	messages.push_back( msg );
	m_worktodo.release();
}

void AsyncOut::Logger::wait()
{	
	m_worktodo.acquire();
}

void AsyncOut::Logger::wait_for( std::chrono::steady_clock::duration timeout )
{	
	m_worktodo.try_acquire_for(timeout);
}


std::list<AsyncOut::Logger::value_type> AsyncOut::Logger::popAll()
{
	std::lock_guard<std::mutex> ml(m_messages);
	std::list<value_type> ret( std::move_iterator(std::begin(messages)),
				               std::move_iterator(std::end(messages)) );
	messages.clear();
	return ret;
}

void AsyncOut::Logger::log()
{
	auto all_messages = popAll();

	for( const auto & m : all_messages ) {
		if( print_line_and_file_info ) {
			std::string file_name = std::filesystem::path(m.file).filename().string();
			std::cout << color_output( m.color, file_name );
			std::cout << ':' << m.line
					  << " ";
		}

		if( !prefix.empty() ) {
			 std::cout << color_output( m.color, DetectLocale::w2out(m.prefix) );
			 std::cout << " ";
		}

		std::string message;

		if( const auto str_ptr (std::get_if<std::string>(&m.message)); str_ptr) {
			message = *str_ptr;
		} else {
			message = DetectLocale::w2out(std::get<std::wstring>(m.message));
		}

		std::cout << message << '\n';
	}
}
