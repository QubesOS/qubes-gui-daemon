// TODO:
// + send_keymap_notify
// + write_message -> .h
// copy verbatim:
    // - process_xevent_focus
    // - process_xevent_key????

const c = @cImport({
    @cInclude("stdio.h");
    @cInclude("stdint.h");
    @cInclude("qubes-gui-protocol.h");
    @cInclude("xside.h");
    @cInclude("libvchan.h");
});

const libvchan_t = c.lib_vchan_t;
const Ghandles = c.Ghandles;
const msg_hdr_t = c.msg_hdr;

extern fn real_write_message(vchan: ?*libvchan_t, hdr: [*c]u8, size: c_int, data: [*c]u8, datasize: c_int) c_int;
extern fn send_keymap_notify(g: *Ghandles) void;

fn write_message(vchan: *libvchan_t, header: msg_hdr_t, body: []const u8) c_int {
    header.untrusted_len = body.len;
    return real_write_message(vchan, @ptrCast([*] u8, &header), @sizeOf(msg_hdr_t), body.ptr, body.len);
}

// fn qubes_daemon_xinput_plug__init(g: *Ghandles) void {


// // init: 

// //      // select xinput events                                                                
// //      XIEventMask xi_mask;                                                                   
// //      xi_mask.deviceid = XIAllMasterDevices; // https://stackoverflow.com/questions/44095001/getting-double-rawkeypress-events-using-xinput2                                            
// //      xi_mask.mask_len = XIMaskLen(XI_LASTEVENT);                                            
// //      if (!(xi_mask.mask = calloc(xi_mask.mask_len, sizeof(char)))) {                        
// //          fputs("Out of memory!\n", stderr);                                                 
// //          exit(1);                                                                           
// //      }                                                                                      
// //      XISetMask(xi_mask.mask, XI_KeyPress);                                                  
// //      XISetMask(xi_mask.mask, XI_KeyRelease);                                                
// //      XISetMask(xi_mask.mask, XI_FocusIn);                                                   
// //      XISetMask(xi_mask.mask, XI_FocusOut);                                                  
                                                                                            
// //      int err = XISelectEvents(g->display, child_win, &xi_mask, 1);                          
// //      if (err) {                                                                             
// //          fprintf(stderr, "Failed to subscribe to XI events. ErrCode: %d\n", err);           
// //          exit(1);                                                                           
// //      }                                                                                      
// //      free(xi_mask.mask);                                                                    
// //      XSync(g->display, False); 
// }

// fn qubes_daemon_xinput_plug__process_event(g: Ghandles, event: XEvent) void {
//     const cookie = &xevent.cookie;

// // 2333:

// //  static void process_xevent(Ghandles * g)                                                  
// //  {                                                                                         
// //      XEvent event_buffer;                                                                  
// //      XGenericEventCookie *cookie = &event_buffer.xcookie;                                  
// //      XNextEvent(g->display, &event_buffer);                                                
// //      if (XGetEventData(g->display, cookie) &&                                              
// //              cookie->type == GenericEvent &&                                               
// //              cookie->extension == g->xi_opcode) {                                          
// //          XIEvent* xi_event = cookie->data; // from test_xi2.c in xinput cli utility        
                                                                                           
// //          switch (xi_event->evtype) {                                                       
// //          // ideally raw input events are better, but I'm relying on X server's built-in event filtering and routing feature here                                                     
// //          case XI_KeyPress:                                                                 
// //          case XI_KeyRelease:                                                               
// //              process_xievent_keypress(g, (XIDeviceEvent *)xi_event);                       
// //              break;                                                                        
// //          case XI_FocusIn:                                                                  
// //          case XI_FocusOut:                                                                 
// //              process_xievent_focus(g, (XILeaveEvent *)xi_event);                           
// //              break;                                                                        
// //          }                                                                                 
// //          XFreeEventData(g->display, cookie);                                               
// //      } else {                                                                              
// //          switch (event_buffer.type) {                                                      
// //          case ReparentNotify:                                                              
// //              process_xevent_reparent(g, (XReparentEvent *) &event_buffer);                 
// //              break;       
// }
