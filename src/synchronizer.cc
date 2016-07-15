//
//  synchronizer.cc
//  P2PSP
//
//  This code is distributed under the GNU General Public License (see
//  THE_GENERAL_GNU_PUBLIC_LICENSE.txt for extending this information).
//  Copyright (C) 2016, the P2PSP team.
//  http://www.p2psp.org
//

#include "synchronizer.h"

namespace p2psp {
    Synchronizer::Synchronizer()
    : io_service_(),
    player_socket_(io_service_),
    acceptor_(io_service_)
    {
      player_port = 15000;
      peer_data = std::vector<std::vector<char> >();
      synchronized=false;
    }

    Synchronizer::~Synchronizer()
    {}


    void Synchronizer::Run(int argc, const char* argv[]) throw(boost::system::system_error)
    {
      boost::program_options::options_description desc("This is the synchronizer node of P2PSP.\n");
      desc.add_options()
      ("help", "Produce this help message and exit.")
      ("peers",boost::program_options::value<std::vector<std::string> >()-> multitoken(),"Peers list");
      boost::program_options::variables_map vm;
      try{
      boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
      if(argc < 2)
      throw std::exception();
      }
      catch(std::exception& e)
      {
        // If the argument passed is unknown, print the list of available arguments
        std::cout<<desc<<std::endl;
      }
      boost::program_options::notify(vm);
      if(vm.count("help"))
      {
        std::cout<< desc <<std::endl;
      }
      if(vm.count("peers"))
      {
        peer_list = &vm["peers"].as<std::vector<std::string> >();
        TRACE(peer_list->size()<< " peers passed to the synchronizer");
        peer_data.resize(peer_list->size());
        // Run the RunThreads function which in turn starts the threads which connect to the peers
        boost::thread t1(&Synchronizer::RunThreads,this);
        t1.join();
      }
    }

    void Synchronizer::RunThreads()
    {
      // Iterarate through the peer_list and start a thread to connect to the peer for every peer in peer_list
      for(std::vector<std::string>::const_iterator it = peer_list->begin();it!=peer_list->end();++it)
      {
        TRACE("Running thread " << it-peer_list->begin()+1 << " of " << peer_list->end() - peer_list->begin() );
        thread_group_.interrupt_all();
        thread_group_.add_thread(new boost::thread(&Synchronizer::ConnectToPeers,this,*it,(it-peer_list->begin())));

      }
      boost::this_thread::sleep(boost::posix_time::milliseconds(2000));
      WaitForThePlayer();
      //thread_group_.add_thread(new boost::thread(&Synchronizer::PlayInitChunks,this));
      boost::this_thread::sleep(boost::posix_time::milliseconds(2000));
      Synchronize();
      boost::this_thread::sleep(boost::posix_time::milliseconds(2000));
      thread_group_.add_thread(new boost::thread(&Synchronizer::PlayChunk,this));
      thread_group_.join_all(); //Wait for all threads to complete
    }

    void Synchronizer::ConnectToPeers(std::string s, int id) throw(boost::system::system_error)
    {
        TRACE("Connecting to " << s);
        std::vector<std::string> fields;
        boost::algorithm::split(fields,s,boost::is_any_of(":"));
        const boost::asio::ip::address hs = boost::asio::ip::address::from_string(fields[0]);
        unsigned short port = boost::lexical_cast<unsigned short>(fields[1]);
        boost::asio::ip::tcp::endpoint peer(hs,port);
        boost::asio::ip::tcp::socket peer_socket (io_service_);
        peer_socket.connect(peer);
        TRACE("Connected to "<< s);
        peer_data[id].resize(1024);
        TRACE("Resized the vector");
        while(1)
        {
        //TRACE("Receiving data from "<< s);
        boost::asio::read(peer_socket,boost::asio::buffer(peer_data[id]));
        peer_data[id].resize(peer_data[id].size()+1024);
        if(synchronized)
        {
          std::vector<char> v (peer_data[id].begin(),peer_data[id].begin()+1024);
          mtx.lock();
          mixed_data.insert(v); //Add 1024 bytes of each peer chunk to the set
          mtx.unlock();
          peer_data[id].erase(peer_data[id].begin(),peer_data[id].begin()+1024);
        }
        }

    }

    void Synchronizer::Synchronize()
    {
        /*Here we start with a search string and keep on increasing its length until we find a constant offset from the haystack
          string. Once we find the offset, we trim the corresponding peer_data vector according to the offset, so that we achieve
          synchronization. Synchronization is a one time process.
        */
        TRACE("Attempting to synchronize peers");
        int start_offset=100,offset=6;
        peer_data[0].erase(peer_data[0].begin(),peer_data[0].begin()+start_offset);
        std::string needle(peer_data[0].begin(),peer_data[0].begin()+offset);
        for(std::vector<std::vector<char> >::iterator it = peer_data.begin()+1; it!=peer_data.end();++it) //Iterating through all the elements of peer_data vector
        {
            std::string haystack (it->begin(),it->end());
            TRACE("Haystack size "<< haystack.size());
            std::size_t found,found_before;
            while((found=haystack.find(needle))!=std::string::npos && found!=found_before)
            {
                offset++;
                needle = std::string(peer_data[0].begin()+start_offset,peer_data[0].begin()+start_offset+offset); //Incremental length of the search string
                found_before=found; //We stop the loop when the found variable no more changes
            }
            if(found == std::string::npos) //If the string matching fails, continue
            {
            TRACE("Synchronization of peer "<< it-peer_data.begin()<<" with peer 0 failed\nIgnoring the peer");
            Synchronize();
            return;
            }
            TRACE("Synchronized peer " << it-peer_data.begin());
            it->erase(it->begin(),it->begin()+found); //Trim the first 'found' bytes of the vector
        }
        synchronized=true;
    }

    void Synchronizer::PlayChunk() throw(boost::system::system_error)
    {
        std::unordered_set<std::vector<char>, VectorHash>::iterator it;
        while((FindNextChunk()))
        {
        TRACE("Writing to the player");
        it=mixed_data.begin();
        boost::asio::write(player_socket_,boost::asio::buffer(*it));
        mtx.lock();
        mixed_data.erase(it);
        mtx.unlock();
        }
      }


    bool Synchronizer::FindNextChunk()
    {
      while(mixed_data.empty())
      boost::this_thread::sleep(boost::posix_time::milliseconds(10));
      return true;
    }

    void Synchronizer::PlayInitChunks() throw(boost::system::system_error)
    {
      unsigned int offset=1024;
      TRACE("Playing initial chunks from peer 1");
      while(!synchronized)
      {
      std::vector<char>::iterator it = peer_data[0].begin();
      std::vector<char> v(it,it+offset);
      boost::asio::write(player_socket_,boost::asio::buffer(v));
      it+=1024;
      }
      TRACE("Synchronization done. Terminating this thread");
    }

    void Synchronizer::WaitForThePlayer()
    {
      boost::asio::ip::tcp::endpoint player_endpoint (boost::asio::ip::tcp::v4(), player_port);
      acceptor_.open(player_endpoint.protocol());
      acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
      acceptor_.bind(player_endpoint);
      acceptor_.listen();
      TRACE("Waiting for the player at (" << player_endpoint.address().to_string() << ","
            << std::to_string(player_endpoint.port())
            << ")");
      acceptor_.accept(player_socket_);
      TRACE("The player is ("
            << player_socket_.remote_endpoint().address().to_string() << ","
            << std::to_string(player_socket_.remote_endpoint().port()) << ")");
      std::string s = "HTTP/1.1 200 OK\r\n\r\n";
      boost::asio::write(player_socket_,boost::asio::buffer(&s[0],s.size()));
    }
}

int main(int argc, const char* argv[])
{
  try {
  p2psp::Synchronizer syn;
  syn.Run(argc, argv);
  }
  catch(boost::system::system_error e)
  {
    TRACE(e.what());
  }
  return -1;
}
