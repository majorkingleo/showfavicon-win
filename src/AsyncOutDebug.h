/**
 * class for printing debugging messages async to stdout
 * @author Copyright (c) 2023 Martin Oberzalek
 *
 * Vendored from cpputils-based tools (see the debug-logging skill): the async
 * frontend and the backend base class. cpputils itself ships only the
 * synchronous OutDebug; these two files are what turns it into a fan-out that
 * the file logger can join.
 */
#pragma once
#include <OutDebug.h>
#include <ColoredOutput.h>
#include <FastDelivery.h>
#include <variant>
#include <mutex>
#include <semaphore>
#include <chrono>

namespace AsyncOut {

struct Data
{
	const char 							   *file;
	unsigned 								line;
	const char 							   *function;
	std::variant<std::string,std::wstring> 	message;
	Tools::ColoredOutput::Color 			color;
	std::wstring 							prefix;	
	std::chrono::utc_clock::time_point 		when = std::chrono::utc_clock::now();
};

class Logger : public Tools::FastDelivery::PublisherNode<Data>, public Tools::OutDebug
{
	std::list<value_type> 	messages;
	std::mutex 				m_messages;
	// Not std::binary_semaphore: its max() is 1, so a second release() before
	// the logger thread drains trips libstdc++'s own assert
	// ("__update <= max() - __old"). Two messages queued in a row is the normal
	// case (the startup path logs several), and with assertions active - our
	// Debug build, and Release, which keeps CPPDEBUG by not defining NDEBUG -
	// that abort is fatal. A counting semaphore with the default, effectively
	// unbounded max is the same "work is waiting" signal without the false stop.
	std::counting_semaphore<> 	m_worktodo{0};
public:

	// called async from Debug publisher.
	void deliver( const value_type & msg );

	virtual void log();

	// wait for data
	void wait();
	void wait_for( std::chrono::steady_clock::duration timeout );	

protected:
	std::list<value_type> popAll();
};

class Debug : public Tools::OutDebug, public Tools::FastDelivery::Publisher<Data,Logger>
{
public:
	Debug( Tools::ColoredOutput::Color color = Tools::ColoredOutput::BRIGHT_YELLOW );

#ifdef __cpp_lib_string_view
  virtual void add( const char *file, unsigned line, const char *function, const std::string_view & s ) override;
  virtual void add( const char *file, unsigned line, const char *function, const std::wstring_view & s ) override;
#else
  virtual void add( const char *file, unsigned line, const char *function, const std::string & s ) override;
  virtual void add( const char *file, unsigned line, const char *function, const std::wstring & s ) override;
#endif

};

} // namespace AsyncOut
