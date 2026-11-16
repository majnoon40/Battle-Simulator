#include "sim/events.hpp"

#include <ostream>

#include "sim/squads.hpp"

namespace sim {

const char* event_type_name(EventType type) noexcept {
  switch (type) {
    case EventType::Death:
      return "death";
    case EventType::Rout:
      return "rout";
    case EventType::Recovery:
      return "recovery";
    case EventType::CommanderDeath:
      return "commander_death";
    case EventType::Succession:
      return "succession";
    case EventType::OrderChange:
      return "order_change";
    case EventType::FlankStart:
      return "flank_start";
    case EventType::HelpStart:
      return "help_start";
    case EventType::PursuitStart:
      return "pursuit_start";
    case EventType::SeekStart:
      return "seek_start";
    case EventType::SquadCollapse:
      return "squad_collapse";
    case EventType::FirstContact:
      return "first_contact";
    case EventType::OrderIssued:
      return "order_issued";
    case EventType::OrderDelivered:
      return "order_delivered";
  }
  return "unknown";
}

std::uint8_t order_event_value(int order) noexcept {
  return static_cast<std::uint8_t>(static_cast<int>(order));
}

void EventRecorder::record(std::uint64_t tick, EventType type,
                           std::uint32_t a, std::uint32_t b) {
  events_.push_back(Event{tick, type, a, b});
  if (out_ != nullptr) {
    write_event_jsonl(*out_, events_.back());
  }
}

std::size_t EventRecorder::count(EventType type) const noexcept {
  std::size_t total = 0;
  for (const Event& event : events_) {
    if (event.type == type) {
      ++total;
    }
  }
  return total;
}

void write_event_jsonl(std::ostream& out, const Event& event) {
  out << "{\"tick\":" << event.tick
      << ",\"type\":\"" << event_type_name(event.type) << '"';
  switch (event.type) {
    case EventType::Death:
      out << ",\"unit\":" << event.a << ",\"killer\":" << event.b;
      break;
    case EventType::Rout:
    case EventType::Recovery:
    case EventType::FlankStart:
    case EventType::HelpStart:
    case EventType::PursuitStart:
    case EventType::SeekStart:
      out << ",\"unit\":" << event.a;
      break;
    case EventType::CommanderDeath:
      out << ",\"commander\":" << event.a << ",\"squad\":" << event.b;
      break;
    case EventType::Succession:
      out << ",\"successor\":" << event.a << ",\"squad\":" << event.b;
      break;
    case EventType::OrderChange:
      out << ",\"squad\":" << event.a << ",\"order\":\""
          << to_string(static_cast<Order>(static_cast<int>(event.b)))
          << "\"";
      break;
    case EventType::SquadCollapse:
      out << ",\"squad\":" << event.a;
      break;
    case EventType::FirstContact:
      out << ",\"unit\":" << event.a;
      break;
    case EventType::OrderIssued:
    case EventType::OrderDelivered:
      out << ",\"squad\":" << event.a << ",\"order\":\""
          << to_string(static_cast<PlayerOrderType>(
                static_cast<int>(event.b)))
          << "\"";
      break;
  }
  out << "}\n";
}

}  // namespace sim
