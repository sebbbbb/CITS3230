/// This file implements our WiFi data link layer.

#include "dll_wifi.h"

#include <cnet.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIFI_MAXDATA 2312	// Define max wifi size.
#define SLOT 40		     	// Define our backoff slot time.

/// This struct type will hold the state for one instance of the WiFi data
/// link layer. The definition of the type is not important for clients.
///
struct dll_wifi_state {
  // The link that this instance of the WiFi protocol is associated with.
  int link;
  
  // A pointer to the function that is called to pass data up to the next layer.
  up_from_dll_fn_ty nl_callback;
  
  // True iff this node is part of the DS (i.e. an access point).
  bool is_ds;
  
  // Iff 0 then link is free, otherwise the link is busy. 
  int busy;

// Add members to represent the WiFi link's state here.
};

/// This struct specifies the format of the control section of a WiFi frame.
struct wifi_control {
  unsigned from_ds : 1;
};

/// This struct specifies the format of a WiFi frame.
///
struct wifi_frame {
  // Control section.
  struct wifi_control control;
  
  // Number of bytes in the payload.
  uint16_t length;
  
  // Address of the receiver.
  CnetNICaddr dest;
  
  // Address of the transmitter.
  CnetNICaddr src;
  
  // CRC32 for the entire frame.
  uint32_t checksum;

  // Number of collisions the frame has experienced.
  int collisions;
  
  // Data must be the last field, because we will truncate the unused area when
  // sending to the physical layer.
  char data[WIFI_MAXDATA];
};

CnetTimerID lasttimer2 = NULLTIMER;	// This timer ID will hold our carrier sense timer.
struct dll_wifi_state *tempstate;	// This will be used to store our wifi state for retransmission use.
struct wifi_frame tempframe;		// This will be used to hold our buffered frame for retransmission when line is busy.
size_t tempframe_length;		// This will be used to hold the size of our frame for retransmission use.

#define WIFI_HEADER_LENGTH (offsetof(struct wifi_frame, data))

static EVENT_HANDLER(backoff) {
  //fprintf(stdout, "node %d: collision %d\n", nodeinfo.address, tempframe.collisions);
  dll_wifi_write(tempstate, tempframe.dest, tempframe.data, tempframe_length);
}

/// Create a new state for an instance of the WiFi data link layer.
///
struct dll_wifi_state *dll_wifi_new_state(int link,
                                          up_from_dll_fn_ty callback,
                                          bool is_ds) {
  // Ensure that the given link exists and is a WLAN link.
  if (link > nodeinfo.nlinks || linkinfo[link].linktype != LT_WLAN) return NULL;
  
  // Allocate memory for the state.
  struct dll_wifi_state *state = calloc(1, sizeof(struct dll_wifi_state));
  
  // Check whether or not the allocation was successful.
  if (state == NULL) return NULL;
  
  // Initialize the members of the structure.
  state->link = link;
  state->nl_callback = callback;
  state->is_ds = is_ds;

  // Call our required event handlers
  CHECK(CNET_set_handler(EV_TIMER2, backoff, 0)); 
  
  return state;
}

/// Delete the given dll_wifi_state. The given state pointer will be invalid
/// following a call to this function.
///
void dll_wifi_delete_state(struct dll_wifi_state *state) {
  // If state does not exist return.
  if (state == NULL) return;
  
  // Free any dynamic memory that is used by the members of the state.
  free(state);
}

/// Write a frame to the given WiFi link.
///
void dll_wifi_write(struct dll_wifi_state *state,
                    CnetNICaddr dest,
                    const char *data,
                    uint16_t length) {
  // If data is empty or length is larger than maximum discard data.
  if (!data || length == 0 || length > WIFI_MAXDATA) return;

  // Check if the link is busy or not
  if(CNET_carrier_sense(state->link) == 1) {
    tempstate = state; // Assign a pointer to our current state
    
    // Copy our data into a temp frame for retransmission.
    memcpy(tempframe.dest, dest, sizeof(CnetNICaddr));
    memcpy(tempframe.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
    memcpy(tempframe.data, data, length);
    tempframe.checksum = CNET_crc32((unsigned char *)&tempframe, sizeof(tempframe));
    tempframe_length = length;
    tempframe.collisions+= 1;	// Increment number of collisions.
    
    srand(time(NULL)); // Create a new seed to be used in our rand function.
    
    int c;	// Create an integer to be used to help generate our random backoff time.
    
    //fprintf(stdout, "node %d, collisions %d, framesize %d\n", nodeinfo.address, tempframe.collisions, (int)(length+WIFI_HEADER_LENGTH));
    
    // If more than 16 collisions discard the frame.
    if(tempframe.collisions == 16) {
      tempframe.collisions = 0;
      return;
    }
    else if(tempframe.collisions >= 10) c = rand() % (int)pow(2, 10);	// Back off for maximum time.
    else if(tempframe.collisions < 10) c = rand() % (int)pow(2, tempframe.collisions);	// Calculate backoff time.
    
    CnetTime backoff = ((CnetTime)SLOT * c);	// Set amount of time to delay.
    lasttimer2 = CNET_start_timer(EV_TIMER2, backoff, 0);	// Start timer.
    return;
  }
  //fprintf(stdout, "node %d: collision on success %d ", nodeinfo.address, tempframe.collisions);
  tempframe.collisions = 0;
  
  // Create a frame and initialize the length field.
  struct wifi_frame frame = (struct wifi_frame) {
    .control = (struct wifi_control) {
      .from_ds = (state->is_ds ? 1 : 0)
    },
    .length = length
  };
  
  // Set the destination and source address.
  memcpy(frame.dest, dest, sizeof(CnetNICaddr));
  memcpy(frame.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
  
  // Copy in the payload.
  memcpy(frame.data, data, length);
  
  // Set the checksum.
  frame.checksum = CNET_crc32((unsigned char *)&frame, sizeof(frame));
  
  // Calculate the number of bytes to send.
  size_t frame_length = WIFI_HEADER_LENGTH + length;
  
  CHECK(CNET_write_physical(state->link, &frame, &frame_length));
  //fprintf(stdout, "node %d: wifi success, framesize is %d\n", nodeinfo.address, (int)frame_length);
}

/// Called when a frame has been received on the WiFi link. This function will
/// retrieve the payload, and then pass it to the callback function that is
/// associated with the given state struct.
///
void dll_wifi_read(struct dll_wifi_state *state,
                   const char *data,
                   size_t length) {
  // If frame is too large then discard.
  if (length > sizeof(struct wifi_frame)) {
    return;
  }
  
  // Treat the data as a WiFi frame.
  const struct wifi_frame *frame = (const struct wifi_frame *)data;
  
  // Ignore WiFi frames received from other APs.
  if (frame->control.from_ds && state->is_ds) {
    printf("\tWiFi: Ignoring frame from access point.\n");
    return;
  }
  
  // Send the frame up to the next layer.
  if (state->nl_callback)
    (*(state->nl_callback))(state->link, frame->data, frame->length);
}
