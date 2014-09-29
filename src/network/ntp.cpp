#include <fc/network/ntp.hpp>
#include <fc/network/udp_socket.hpp>
#include <fc/network/resolve.hpp>
#include <fc/network/ip.hpp>
#include <fc/thread/thread.hpp>

#include <stdint.h>
#include "../byteswap.hpp"

#include <atomic>
#include <array>

namespace fc
{
  namespace detail {

     class ntp_impl 
     {
        public:
           /** vector < host, port >  */
           fc::thread                                       _ntp_thread;
           std::vector< std::pair< std::string, uint16_t> > _ntp_hosts;
           fc::future<void>                                 _read_loop_done;
           udp_socket                                       _sock;
           uint32_t                                         _request_interval_sec;
           fc::time_point                                   _last_request_time;

           std::atomic_bool                                 _last_ntp_delta_initialized;
           std::atomic<int64_t>                             _last_ntp_delta_microseconds;


           fc::future<void>                                 _request_time_task_done;

           ntp_impl() :
            _ntp_thread("ntp"),
            _request_interval_sec( 60*60 /* 1 hr */),
            _last_ntp_delta_microseconds(0)
           { 
              _last_ntp_delta_initialized = false;
              _ntp_hosts.push_back( std::make_pair( "pool.ntp.org",123 ) );
           } 

           ~ntp_impl() 
           { 
             _ntp_thread.quit(); //TODO: this can be removed once fc::threads call quit during destruction
           }

           void request_now()
           {
               assert(_ntp_thread.is_current());
               for( auto item : _ntp_hosts )
               {
                  try 
                  {
                     wlog( "resolving... ${r}", ("r", item) );
                     auto eps = resolve( item.first, item.second );
                     for( auto ep : eps )
                     {
                        ilog( "sending request to ${ep}", ("ep",ep) );
                        std::shared_ptr<char> send_buffer(new char[48], [](char* p){ delete[] p; });
                        std::array<unsigned char, 48> packet_to_send { {010,0,0,0,0,0,0,0,0} };
                        memcpy(send_buffer.get(), packet_to_send.data(), packet_to_send.size());
                        _last_request_time = fc::time_point::now();
                        _sock.send_to( send_buffer, packet_to_send.size(), ep );
                        break;
                     }
                  } 
                  catch (const fc::canceled_exception&)
                  {
                    throw;
                  }
                  // this could fail to resolve but we want to go on to other hosts..
                  catch ( const fc::exception& e )
                  {
                      elog( "${e}", ("e",e.to_detail_string() ) ); 
                  }
               }
           } // request_now

           //started for first time in ntp() constructor, canceled in ~ntp() destructor
           void request_time_task()
           {
              assert(_ntp_thread.is_current());
              request_now();
              if (!_request_time_task_done.canceled())
                _request_time_task_done = schedule( [=](){ request_time_task(); }, 
                                                    fc::time_point::now() + fc::seconds(_request_interval_sec), 
                                                    "request_time_task" );
           } // request_loop

           void start_read_loop()
           {
              _read_loop_done = _ntp_thread.async( [this](){ read_loop(); }, "ntp_read_loop" );
           }

           void read_loop()
           {
              assert(_ntp_thread.is_current());

              uint32_t receive_buffer_size = sizeof(uint64_t) * 1024;
              std::shared_ptr<char> receive_buffer(new char[receive_buffer_size], [](char* p){ delete[] p; });
              uint64_t* recv_buf = (uint64_t*)receive_buffer.get();

              //outer while to restart read-loop if exception is thrown while waiting to receive on socket.
              while( !_read_loop_done.canceled() )
              {
                // if you start the read while loop here, the recieve_from call will throw "invalid argument" on win32,
                // so instead we start the loop after making our first request
                try 
                {
                  _sock.open();
                  request_time_task();

                  while( !_read_loop_done.canceled() )
                  {
                     fc::ip::endpoint from;
                     try
                     {
                       _sock.receive_from( receive_buffer, receive_buffer_size, from );
                     } FC_RETHROW_EXCEPTIONS(error, "Error reading from NTP socket");

                     uint64_t receive_timestamp_net_order = recv_buf[4];
                     uint64_t receive_timestamp_host = bswap_64(receive_timestamp_net_order);
                     uint32_t fractional_seconds = receive_timestamp_host & 0xffffffff;
                     uint32_t microseconds = (uint32_t)(((((uint64_t)fractional_seconds) * 1000000) + (UINT64_C(1)<<31)) >> 32);
                     uint32_t seconds_since_1900 = receive_timestamp_host >> 32;
                     uint32_t seconds_since_epoch = seconds_since_1900 - 2208988800;

                     if( fc::time_point::now() - _last_request_time > fc::seconds(1) )
                        request_now();
                     else
                     {
                        auto ntp_time = (fc::time_point() + fc::seconds(seconds_since_epoch) + fc::microseconds(microseconds));
                        if( ntp_time - fc::time_point::now() < fc::seconds(60*60*24) &&
                            fc::time_point::now() - ntp_time < fc::seconds(60*60*24) )
                        {
                           _last_ntp_delta_microseconds = (ntp_time - fc::time_point::now()).count();
                           _last_ntp_delta_initialized = true;
                           fc::microseconds ntp_delta_time = fc::microseconds(_last_ntp_delta_microseconds);
                           wlog("ntp_delta_time updated to ${delta_time}", ("delta_time",ntp_delta_time) );
                        }
                        else
                           elog( "NTP time is way off ${time}", ("time",ntp_time)("local",fc::time_point::now()) );
                     }
                  }
               } // try
               catch (fc::canceled_exception)
               {
                 throw;
               }
               catch (...)
               {
                 //swallow any other exception and restart loop
                 elog("unexpected exception in read_loop, going to restart it.");
               }
               _sock.close();
               fc::usleep(fc::seconds(_request_interval_sec));
             } //outer while loop
             wlog("exiting ntp read_loop");
          } //end read_loop()
     }; //ntp_impl

  } // namespace detail



  ntp::ntp()
  :my( new detail::ntp_impl() )
  {
     my->start_read_loop();
  }

  ntp::~ntp()
  {
    my->_ntp_thread.async([=](){
      try
      {
        my->_request_time_task_done.cancel_and_wait("ntp object is destructing");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while shutting down NTP's request_time_task, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while shutting down NTP's request_time_task, ignoring" );
      }
      
      try 
      {
        my->_read_loop_done.cancel_and_wait("ntp object is destructing");
      } 
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while shutting down NTP's read_loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while shutting down NTP's read_loop, ignoring" );
      }

    }, "ntp_shutdown_task").wait();
  }


  void ntp::add_server( const std::string& hostname, uint16_t port)
  {
     my->_ntp_thread.async( [=](){ my->_ntp_hosts.push_back( std::make_pair(hostname,port) ); }, "add_server" ).wait();
  }

  void ntp::set_request_interval( uint32_t interval_sec )
  {
     my->_request_interval_sec = interval_sec;
  }

  void ntp::request_now()
  {
     my->_ntp_thread.async( [=](){ my->request_now(); }, "request_now" ).wait();
  }

  optional<time_point> ntp::get_time()const
  {
     if( my->_last_ntp_delta_initialized )
        return fc::time_point::now() + fc::microseconds(my->_last_ntp_delta_microseconds);
     return optional<time_point>();
  }

}
