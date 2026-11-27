// Spartan Test trackers for Xenia (PSYCHOsmurf / Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
#ifndef XENIA_UWP_SPARTAN_TRACKER_H_
#define XENIA_UWP_SPARTAN_TRACKER_H_

namespace xe {
class Emulator;
}

namespace spartan {

// Writes the trackers' start line, installs the crash handler and starts the memory and audio watch (one thread,
// for the life of the app). Call once, after the emulator is set up.
void StartTrackers(xe::Emulator* emulator);

// The report card once more, when Xenia closes (before its last log flush). Nothing when no game showed a frame.
void FinalReport();

// The report card also when the app goes to the background or is suspended (a quit from the Xbox button ends the
// app without Xenia's own shutdown). Call on the window's thread, after StartTrackers.
void WatchAppLeaving();

}  // namespace spartan

#endif  // XENIA_UWP_SPARTAN_TRACKER_H_
