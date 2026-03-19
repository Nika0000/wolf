#include <chrono>
#include <immer/set.hpp>
#include <immer/vector_transient.hpp>
#include <sessions/handlers.hpp>
#include <thread>

namespace wolf::core::sessions {

using namespace std::chrono_literals;

/**
 * @brief Starts a background watchdog thread that monitors sessions for idle timeouts.
 *
 * For each session that has an `idle_timeout_seconds` configured, the watchdog fires a
 * `StopStreamEvent` once the session has been idle for at least that many seconds.
 * A `StopStreamEvent` handler is registered to clean up tracking state when a session ends.
 *
 * @returns Event bus handlers that must be kept alive for the lifetime of the watchdog.
 */
immer::vector<immer::box<events::EventBusHandlers>>
setup_idle_timeout_watchdog(const state::SessionsAtoms &sessions,
                            std::shared_ptr<events::EventBusType> ev_bus) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  // Thread-safe set of session IDs for which a StopStreamEvent has already been fired
  auto stop_fired_sessions = std::make_shared<immer::atom<immer::set<std::string>>>();

  // Event-driven cleanup: remove session from tracking as soon as it stops
  handlers.push_back(ev_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [stop_fired_sessions](const immer::box<events::StopStreamEvent> &ev) {
        stop_fired_sessions->update([&ev](const immer::set<std::string> &s) { return s.erase(ev->session_id); });
      }));

  std::thread([sessions, ev_bus, stop_fired_sessions]() {
    while (true) {
      auto running = sessions->load();
      auto now = std::chrono::steady_clock::now();
      auto fired = stop_fired_sessions->load();

      for (const auto &session : running.get()) {
        if (!session.idle_timeout_seconds || *session.idle_timeout_seconds <= 0) {
          continue;
        }

        if (fired->count(session.session_id)) {
          continue;
        }

        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        auto idle_for_ns = now_ns - session.last_input_at_ns->load();
        auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::seconds(*session.idle_timeout_seconds))
                              .count();
        if (idle_for_ns >= timeout_ns) {
          logs::log(logs::info,
                    "[SESSION] Idle timeout reached for session {} ({}s), stopping stream",
                    session.session_id,
                    *session.idle_timeout_seconds);
          stop_fired_sessions->update(
              [&session](const immer::set<std::string> &s) { return s.insert(session.session_id); });
          ev_bus->fire_event(
              immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session.session_id}));
        }
      }

      std::this_thread::sleep_for(1s);
    }
  }).detach();

  return handlers.persistent();
}

} // namespace wolf::core::sessions
