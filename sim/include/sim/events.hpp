#pragma once

#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <vector>

namespace sim {

// Battle event types (M10 Part 2a). One Event is one recorded happening;
// ids are stable unit ids (Unit::id) and squad indices. Events are
// appended in deterministic order (phase, then unit/squad index order),
// so the same battle always records the same event stream.
enum class EventType : std::uint8_t {
  Death,           // a: victim id, b: killer id (0 = no killer recorded)
  Rout,            // a: unit id
  Recovery,        // a: unit id (rout ended)
  CommanderDeath,  // a: commander id, b: squad index
  Succession,      // a: successor id, b: squad index
  OrderChange,     // a: squad index, b: order enum value
  FlankStart,      // a: unit id (first tick of a Flank decision run)
  HelpStart,       // a: unit id
  PursuitStart,    // a: unit id (starts chasing a routed enemy)
  SeekStart,       // a: unit id (contact seek escalates to the estimate)
  SquadCollapse,   // a: squad index (its last member just died)
  FirstContact,    // a: unit id (first unit to see an enemy; once per battle)
  OrderIssued,     // m11b: a: squad index, b: player order type value
  OrderDelivered,  // m11b: a: squad index, b: player order type value
};

[[nodiscard]] const char* event_type_name(EventType type) noexcept;

std::uint8_t order_event_value(int order) noexcept;  // Order enum -> value

struct Event {
  std::uint64_t tick = 0;  // 1-based tick the event happened on
  EventType type = EventType::Death;
  std::uint32_t a = 0;
  std::uint32_t b = 0;
};

// Records events. attach_ostream enables JSONL streaming as they happen
// (the caller owns the stream and must keep it open); events are also
// kept in memory for the report generator. A null recorder (or one that
// is never attached to a Simulation) changes nothing — battles with and
// without a recorder are byte-identical.
class EventRecorder {
 public:
  void attach_ostream(std::ostream* out) { out_ = out; }

  void record(std::uint64_t tick, EventType type, std::uint32_t a,
              std::uint32_t b = 0);

  [[nodiscard]] const std::vector<Event>& events() const noexcept {
    return events_;
  }
  // Number of recorded events of one type (report + tests).
  [[nodiscard]] std::size_t count(EventType type) const noexcept;

 private:
  std::vector<Event> events_;
  std::ostream* out_ = nullptr;
};

// Writes one event as a single JSONL object (no trailing newline games:
// exactly one '\n' after each object).
void write_event_jsonl(std::ostream& out, const Event& event);

}  // namespace sim
