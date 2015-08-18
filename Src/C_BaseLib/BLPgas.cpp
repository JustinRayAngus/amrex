
#include <BLassert.H>
#include <BLPgas.H>

#ifdef BL_USE_CXX11
#include <unordered_map>
#else
#include <map>
#endif

namespace
{
#ifdef BL_USE_CXX11
  typedef std::unordered_multimap< upcxx::rank_t, BLPgas::SendInfo > pgas_send_info_map_t;
#else
  typedef std::multimap< upcxx::rank_t, BLPgas::SendInfo > pgas_send_info_map_t;
#endif
  static pgas_send_info_map_t pgas_send_info_map;
}

namespace BLPgas {
  upcxx::event fb_send_event;
  upcxx::event fb_recv_event;
  int          fb_send_counter;

  upcxx::event fpb_send_event;
  upcxx::event fpb_recv_event;
  int          fpb_send_counter;
}

/**
 * \brief send a message using PGAS one-sided communication
 *
 * BLPgas::Send is a drop-in replacement of MPI_Isend for non-blocking
 * communication.  It uses an Active Receive (Sender Side Tag Matching)
 * protocol to perform point-to-point message passing as follows.  The
 * receiver first sends its recv request (recv buffer address, message
 * size and tag) to the sender.  The sender does a simple tag matching with
 * the recv request and then initiate an one-sided put operation to transfer
 * the message from the send buffer to the recv buffer.
 *
 * @param src the source (send) buffer address
 * @param dst the destination (recv) buffer address
 * @param nbytes number of bytes for the message
 * @param SeqNum the BoxLib internal sequence number of message
 * @param signal_event for notifying the receiver when the data transfer is done
 * @param done_event for notifying the sender when the data transfer is done
 * @param send_counter increment the counter only if the message is sent out
 */
void
BLPgas::Send(upcxx::global_ptr<void> src,
             upcxx::global_ptr<void> dst,
             size_t nbytes,
             int SeqNum,
             upcxx::event *signal_event,
             upcxx::event *done_event,
             int *send_counter)
{
#ifdef DEBUG
  std::cout << "myrank " << myrank() << " pgas_send: src " << src
            << " dst " << dst << " nbytes " << nbytes
            << " SeqNum " << SeqNum << " signal_event " << signal_event
            << " done_event " << done_event << " send_counter " << send_counter
            << "\n";
#endif

  // We use dst_rank as the key for the hash table
  std::pair <pgas_send_info_map_t::iterator, pgas_send_info_map_t::iterator> ret;
  ret = pgas_send_info_map.equal_range(dst.where());

  // try to match the SeqNum
  for (pgas_send_info_map_t::iterator it = ret.first;
       it != ret.second;
       ++it) {
    SendInfo& send_info = it->second;
    if (SeqNum == send_info.SeqNum) {
      // found the matched message
      // Check if data size matches
      assert(nbytes == send_info.nbytes);

      // Fire off the non-blocking one-sided communication (put)
      // If the receive request comes first, then the src_ptr in the existing
      // send_info entry should be NULL; otherwise, if the send request comes
      // first, then the dst_ptr must be NULL.  If neither is true, then there
      // must be something wrong!
      if (send_info.src_ptr == NULL) {
        // pgas_send request from Recv came earlier
        send_info.src_ptr = src;
        send_info.done_event = done_event;
        send_info.send_counter = send_counter;
#ifdef DEBUG
        std::cout << "myrank " << myrank() << " send found SeqNum match "
                  << SeqNum << "\n";
#endif
      } else {
        // pgas_send request from Send came earlier
        assert(send_info.dst_ptr == NULL);
        send_info.dst_ptr = dst;
        send_info.signal_event = signal_event;
#ifdef DEBUG
        std::cout << "myrank " << myrank() << " recv found SeqNum match "
                  << SeqNum << "\n";
#endif
      }

      send_info.check();

      async_copy_and_signal(send_info.src_ptr,
                            send_info.dst_ptr,
                            send_info.nbytes,
                            send_info.signal_event,
                            send_info.done_event);

      (*send_info.send_counter)++;
      // Delete the send_info from the map
      pgas_send_info_map.erase(it);
      return;
    }
  }

  // Can't find the send_info entry in the hash table
  // Create a new send_info and store the receiver part of it
  SendInfo send_info {src, dst, nbytes, SeqNum, signal_event, done_event, send_counter};
  pgas_send_info_map.insert(std::pair<upcxx::rank_t, SendInfo>(dst.where(), send_info));
}
