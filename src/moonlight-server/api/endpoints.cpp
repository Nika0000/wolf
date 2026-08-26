#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <core/docker.hpp>
#include <moonlight/protocol.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

void UnixSocketServer::endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // curl -N --unix-socket /tmp/wolf.sock http://localhost/api/v1/events
  state_->sockets.push_back(socket);
  send_http(socket,
            200,
            {{"Content-Type: text/event-stream"}, {"Connection: keep-alive"}, {"Cache-Control: no-cache"}},
            ""); // Inform clients this is going to be SS
}

void UnixSocketServer::endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto requests = std::vector<PendingPairClient>();
  for (auto [secret, pair_request] : *(state_->app_state)->pairing_atom->load()) {
    requests.push_back({.pair_secret = secret, .client_ip = pair_request->client_ip});
  }
  send_http(socket, 200, rfl::json::write(PendingPairRequestsResponse{.requests = requests}));
}

void UnixSocketServer::endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<PairRequest>(req.body);
  if (event) {
    if (auto pair_request = state_->app_state->pairing_atom->load()->find(event.value().pair_secret)) {
      pair_request->get().user_pin->set_value(event.value().pin.value()); // Resolve the promise
      state_->app_state->pairing_atom->update(
          [pair_secret = event.value().pair_secret](auto pairing_map) { return pairing_map.erase(pair_secret); });
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid pair secret: {}", event.value().pair_secret);
      auto res = GenericErrorResponse{.error = "Invalid pair secret"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = PairedClientsResponse{.success = true};
  auto clients = state_->app_state->config->paired_clients->load();
  for (const config::PairedClient &client : clients.get()) {
    res.clients.push_back(PairedClient{.client_id = std::to_string(state::get_client_id(client)),
                                       .app_state_folder = client.app_state_folder,
                                       .settings = client.settings});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  try {
    auto payload_result = rfl::json::read<UnpairClientRequest>(req.body);
    if (!payload_result) {
      auto res = GenericErrorResponse{.error = "Invalid request format"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    const auto &payload = payload_result.value(); // Unwrap the Result
    auto client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
    if (!client) {
      auto res = GenericErrorResponse{.error = "Client not found"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    state::unpair(this->state_->app_state->config, *client);

    auto res = GenericSuccessResponse{.success = true};
    send_http(socket, 200, rfl::json::write(res));
  } catch (const std::exception &e) {
    auto res = GenericErrorResponse{.error = e.what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps.push_back(rfl::Reflector<events::App>::to(app, this->state_->app_state->event_bus));
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps | //
                         ranges::views::filter(
                             [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                         ranges::to<immer::vector<immer::box<events::App>>>();
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles.push_back(rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus)));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::remove_if([&p](const immer::box<events::Profile> &profile) {
                                 return profile.get().id == p.id;
                               }) | //
                               ranges::to<state::ProfilesList>());
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_ServerInfo(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = ServerInfoResponse{.success = true};

  auto cfg = state_->app_state->config;

  res.unique_id = cfg->uuid;
  res.hostname = cfg->hostname;
  res.app_version = moonlight::M_VERSION;
  res.gfe_version = moonlight::M_GFE_VERSION;

  int codec_support = moonlight::VIDEO_FORMAT_H264;
  int max_luma_pixels = 0;
  if (cfg->support_hevc) {
    max_luma_pixels = 1869449984;
    codec_support |= moonlight::VIDEO_FORMAT_H265;
  }
  if (cfg->support_av1) {
    codec_support |= moonlight::VIDEO_FORMAT_AV1_MAIN8;
  }

  res.max_luma_pixels = max_luma_pixels;
  res.codec_mode_support = codec_support;

  res.rtsp_port = state::get_port(state::RTSP_SETUP_PORT);

  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    res.sessions.push_back(rfl::Reflector<events::StreamSession>::from(session));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionCreate(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session_req = rfl::json::read<StreamSessionCreateRequest>(req.body);
  if (!session_req) {
    logs::log(logs::warning, "[API] Invalid request: {} - {}", req.body, session_req.error().what());
    auto res = GenericErrorResponse{.error = session_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
    return;
  }

  auto ss = session_req.value();

  // Get defaults from moonlight profile if available
  auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
  immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
  immer::box<events::App> sample_app = apps.front();

  std::string default_video_buffer_caps = sample_app->video_producer_buffer_caps;
  std::string default_h264_pipeline = sample_app->h264_gst_pipeline;
  std::string default_hevc_pipeline = sample_app->hevc_gst_pipeline;
  std::string default_av1_pipeline = sample_app->av1_gst_pipeline;
  std::string default_render_node = sample_app->render_node;
  std::string default_opus_pipeline = sample_app->opus_gst_pipeline;

  // Create the runner from user-provided configuration
  auto runner = state::get_runner(ss.runner.value(), this->state_->app_state->event_bus);

  // Create the app from user-provided configuration (not from moonlight profiles)
  immer::box<events::App> app = events::App{
      .base = {.title = "user-created", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

      .video_producer_buffer_caps = ss.video_producer_buffer_caps.get().value_or(default_video_buffer_caps),

      .h264_gst_pipeline = ss.h264_gst_pipeline.get().value_or(default_h264_pipeline),
      .hevc_gst_pipeline = ss.hevc_gst_pipeline.get().value_or(default_hevc_pipeline),
      .av1_gst_pipeline = ss.av1_gst_pipeline.get().value_or(default_av1_pipeline),

      .render_node = ss.render_node.get().value_or(default_render_node),
      .opus_gst_pipeline = ss.opus_gst_pipeline.get().value_or(default_opus_pipeline),
      .start_virtual_compositor = ss.start_virtual_compositor.get().value_or(true),
      .start_audio_server = ss.start_audio_server.get().value_or(true),

      .runner = runner};

  // Handle client - either provided or create a dummy one
  config::PairedClient choosen_client;
  if (ss.client_id.get()) {
    auto client = state::get_client_by_id(this->state_->app_state->config, ss.client_id.get().value());
    if (!client) {
      logs::log(logs::warning, "[API] Invalid client_id: {}", ss.client_id.get().value());
      auto res = GenericErrorResponse{.error = "Invalid client_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }
    choosen_client = *client;
  } else {
    // Create a dummy client
    choosen_client = {.client_cert = "",
                      .app_state_folder = ss.app_state_folder.get().value_or(state::gen_uuid()),
                      .settings = ss.client_settings.get().value_or(config::ClientSettings{})};
  }

  // Create the stream session
  auto new_session = state::create_stream_session(
      state_->app_state,
      *app,
      choosen_client,
      moonlight::DisplayMode{.width = ss.video_width.value(),
                             .height = ss.video_height.value(),
                             .refreshRate = ss.video_refresh_rate.value(),
                             .hevc_supported = state_->app_state->config->support_hevc,
                             .av1_supported = state_->app_state->config->support_av1},
      ss.audio_channel_count.value(),
      ss.aes_key.value(),
      ss.aes_iv.value());

  new_session->ip = ss.client_ip.value();
  new_session->rtsp_fake_ip = ss.rtsp_fake_ip.value();
  if (auto timeout_seconds = ss.idle_timeout_seconds.get(); timeout_seconds && *timeout_seconds > 0) {
    new_session->idle_timeout_seconds = *timeout_seconds;
  }

  // Add session to running sessions
  state_->app_state->running_sessions->update(
      [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });

  // Fire the event
  state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

  auto res = StreamSessionCreated{.success = true, .session_id = new_session->session_id};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    immer::box<events::App> choosen_app;
    auto ss = session.value();
    if (auto app_id = ss.app_id) {
      auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, *app_id);
      if (!app) {
        logs::log(logs::warning, "[API] Invalid app_id: {}", *app_id);
        auto res = GenericErrorResponse{.error = "Invalid app_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_app = *app;
    } else {
      auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
      if (!moonlight_profile) {
        logs::log(logs::warning, "[API] No moonlight profile found, unable to automatically create an app.");
        auto res = GenericErrorResponse{.error = "No moonlight profile found"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
      immer::box<events::App> sample_app = apps.front();
      choosen_app = events::App{
          .base = {.title = "dummy", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

          .video_producer_buffer_caps = sample_app->video_producer_buffer_caps,

          .h264_gst_pipeline = sample_app->h264_gst_pipeline,
          .hevc_gst_pipeline = sample_app->hevc_gst_pipeline,
          .av1_gst_pipeline = sample_app->av1_gst_pipeline,

          .render_node = sample_app->render_node,
          .opus_gst_pipeline = sample_app->opus_gst_pipeline,
          .start_virtual_compositor = ss.start_virtual_compositor.value_or(true),
          .start_audio_server = ss.start_audio_server.value_or(true),

          .runner = std::make_shared<process::RunProcess>(state_->app_state->event_bus,
                                                          "sh -c \"while :; do echo 'running...'; sleep 10; done\"")};
    }

    config::PairedClient choosen_client;
    if (auto client_id = ss.client_id) {
      auto client = state::get_client_by_id(this->state_->app_state->config, *client_id);
      if (!client) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        auto res = GenericErrorResponse{.error = "Invalid client_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_client = *client;
    } else {
      // Create a dummy client
      choosen_client = {.client_cert = "", .app_state_folder = state::gen_uuid(), .settings = {}};
    }

    choosen_client.settings = ss.client_settings.value_or(config::ClientSettings{});

    auto new_session = state::create_stream_session( //
        state_->app_state,
        choosen_app,
        choosen_client,
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv);
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;
    if (ss.idle_timeout_seconds && *ss.idle_timeout_seconds > 0) {
      new_session->idle_timeout_seconds = *ss.idle_timeout_seconds;
    }

    state_->app_state->running_sessions->update(
        [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = new_session->session_id};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    const auto &session_id = start_req.value().session_id;
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      video_session.session_id = session_id;
      if (video_session.render_node.empty()) {
        video_session.render_node = session->app->render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id;
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RunnerPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto pause_req = rfl::json::read<RunnerPauseRequest>(req.body);
  if (pause_req) {
    auto sessions = state_->app_state->running_sessions->load();
    const auto &session_id = pause_req.value().session_id;
    if (auto stream_session = state::get_session_by_id(sessions.get(), session_id)) {
      // Freeze the idle timeout: a session paused on purpose shouldn't be reaped by the watchdog
      stream_session->paused->store(true);
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::RunnerPauseEvent>(events::RunnerPauseEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, pause_req.error().what());
    auto res = GenericErrorResponse{.error = pause_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

/**
 * Unpauses the runner's container (see RunnerResumeEvent handlers, e.g. in runners/docker.cpp), clears
 * the session's paused state and re-starts its idle timeout countdown. The video/audio pipelines are
 * left untouched: freezing the container also freezes frame production, so there's nothing to tear
 * down or recreate.
 */
void UnixSocketServer::endpoint_RunnerResume(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto resume_req = rfl::json::read<RunnerResumeRequest>(req.body);
  if (resume_req) {
    auto sessions = state_->app_state->running_sessions->load();
    const auto &session_id = resume_req.value().session_id;
    if (auto stream_session = state::get_session_by_id(sessions.get(), session_id)) {
      stream_session->paused->store(false);
      // Restart the idle countdown from now, so that a resumed session gets a full timeout window
      stream_session->last_input_at_ns->store(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count());
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::RunnerResumeEvent>(events::RunnerResumeEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, resume_req.error().what());
    auto res = GenericErrorResponse{.error = resume_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    const auto &session_id = session.value().session_id;
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_AppStateDelete(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto delete_req = rfl::json::read<AppStateDeleteRequest>(req.body);
  if (!delete_req) {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, delete_req.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = delete_req.error().what()}));
    return;
  }

  auto client = state::get_client_by_id(state_->app_state->config, delete_req.value().client_id.value());
  if (!client) {
    logs::log(logs::warning, "[API] Invalid client_id: {}", delete_req.value().client_id.value());
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Invalid client_id"}));
    return;
  }

  auto app = state::get_moonlight_app_by_id(state_->app_state->config, delete_req.value().app_id.value());
  if (!app) {
    logs::log(logs::warning, "[API] Invalid app_id: {}", delete_req.value().app_id.value());
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Invalid app_id"}));
    return;
  }

  // Mirrors the layout create_stream_session() uses, so this resolves to the same folder whether
  // or not a session for this client/app pair is currently running.
  auto state_folder = (std::filesystem::path(state_->app_state->host->local_base_state_folder) /
                       client->app_state_folder / app.value()->base.title)
                          .string();

  auto sessions = state_->app_state->running_sessions->load();
  auto running_session =
      std::find_if(sessions.get().begin(), sessions.get().end(), [&state_folder](const events::StreamSession &s) {
        return s.app_local_state_folder == state_folder;
      });

  if (running_session == sessions.get().end()) {
    // Nothing is running against this state folder, just delete it directly.
    try {
      std::filesystem::remove_all(state_folder);
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "[API] Failed to remove state folder {}: {}", state_folder, e.what());
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    return;
  }

  auto session_id = running_session->session_id;

  // Ask the runner to tear down and force-remove the container; running_sessions is updated by the
  // existing StopStreamEvent handler, and docker.cpp only fires DockerContainerStopped once any
  // requested removal has completed, so we wait for that to delete the on-disk state folder.
  auto stopped_promise = std::make_shared<std::promise<void>>();
  auto stopped_future = stopped_promise->get_future();
  auto stopped_handler = state_->app_state->event_bus->register_handler<immer::box<events::DockerContainerStopped>>(
      [session_id, stopped_promise](const immer::box<events::DockerContainerStopped> &ev) {
        if (ev->session_id == session_id) {
          stopped_promise->set_value();
        }
      });

  state_->app_state->event_bus->fire_event(immer::box<events::StopStreamEvent>(
      events::StopStreamEvent{.session_id = session_id, .delete_container = true}));

  std::thread([this, session_id, state_folder, stopped_future = std::move(stopped_future),
              stopped_handler = std::move(stopped_handler)]() mutable {
    // Bounded: runners that never fire DockerContainerStopped (e.g. a plain process runner) would
    // otherwise leave this thread, and the state folder, around forever.
    if (stopped_future.wait_for(std::chrono::seconds(25)) != std::future_status::ready) {
      logs::log(logs::warning, "[API] Timed out waiting for session {} to stop, deleting its state anyway", session_id);
    }
    stopped_handler.unregister();

    try {
      std::filesystem::remove_all(state_folder);
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "[API] Failed to remove state folder {}: {}", state_folder, e.what());
    }
  }).detach();

  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    const auto &session_id = input_request.value().session_id;
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto pkt_parsed = crypto::hex_to_str(hex_pkt);
      control::INPUT_PKT *input_pkt = reinterpret_cast<control::INPUT_PKT *>(pkt_parsed.data());
      control::handle_input(session.value(), {}, input_pkt);

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerExec(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto exec_request = rfl::json::read<RunnerExecRequest>(req.body);
  if (!exec_request) {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, exec_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = exec_request.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = exec_request.value().session_id.get();
  if (!state::get_session_by_id(sessions.get(), session_id)) {
    logs::log(logs::warning, "[API] Invalid session_id: {}", session_id);
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  auto containers = docker_api.get_containers();
  auto suffix = fmt::format("_{}", session_id);
  auto container = std::find_if(containers.begin(), containers.end(), [&suffix](const docker::Container &c) {
    return c.name.size() >= suffix.size() && c.name.compare(c.name.size() - suffix.size(), suffix.size(), suffix) == 0;
  });
  if (container == containers.end()) {
    logs::log(logs::warning, "[API] No running container found for session_id: {}", session_id);
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "No running container found for session"}));
    return;
  }

  auto container_id = container->id;
  auto command_vec = exec_request.value().command.get();
  auto user = exec_request.value().user.get().value_or("root");
  auto stream = exec_request.value().stream.get().value_or(false);

  if (stream) {
    // The exec can be long-running (e.g. waiting on user interaction), stream each output chunk to the client
    // as soon as it's produced instead of buffering until the process exits.
    std::thread([this, socket, container_id, command_vec, user]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      std::vector<std::string_view> command(command_vec.begin(), command_vec.end());

      bool first_send = true;
      auto send_chunk = [this, socket, &first_send](std::string_view chunk) {
        if (chunk.empty()) {
          return;
        }
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
          first_send = false;
        }
        send_data(socket, rfl::json::write(RunnerExecOutputEvent{.output = std::string(chunk)}) + "\r\n");
      };

      if (auto exit_code = docker_api.exec_stream(container_id, command, send_chunk, user)) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        send_data(socket,
                 rfl::json::write(RunnerExecResponse{
                     .success = *exit_code == 0, .exit_code = *exit_code }) +
                     "\r\n");
      } else if (first_send) {
        send_http(socket,
                 500,
                 rfl::json::write(GenericErrorResponse{.error = "Failed to execute command in container"}));
      } else {
        send_data(socket,
                 rfl::json::write(GenericErrorResponse{.error = "Failed to execute command in container"}) + "\r\n");
      }
    }).detach();
  } else {
    std::thread([this, socket, container_id, command_vec, user]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      std::vector<std::string_view> command(command_vec.begin(), command_vec.end());

      if (auto result = docker_api.exec_capture(container_id, command, user)) {
        send_http(socket,
                 200,
                 rfl::json::write(RunnerExecResponse{.success = result->exit_code == 0,
                                                      .exit_code = result->exit_code,
                                                      .output = result->output}));
      } else {
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to execute command in container"}));
      }
    }).detach();
  }
}

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();
    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get().value_or(""),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = event.value().video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration),
                .motion_controller_override = client_settings.motion_controller_override.value_or(
                    default_client_settings.motion_controller_override)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus)};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(20));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session =
        state::get_session_by_id(this->state_->app_state->running_sessions->load(), event.value().session_id);
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload_result = rfl::json::read<UpdateClientSettingsRequest>(req.body);
  if (!payload_result) {
    auto res = GenericErrorResponse{.error = "Invalid request format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  const auto &payload = payload_result.value();
  auto current_client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
  if (!current_client) {
    auto res = GenericErrorResponse{.error = "Client not found"};
    send_http(socket, 404, rfl::json::write(res));
    return;
  }

  // Edit only the settings that are being passed in the payload
  auto current_settings = current_client->settings;
  auto new_settings = payload.settings.get().value_or(PartialClientSettings{});
  auto merged_client = config::PairedClient{
      .client_cert = current_client->client_cert, // Immutable, changing this would mean a new client
      .app_state_folder = payload.app_state_folder.get().value_or(current_client->app_state_folder),
      .settings = config::ClientSettings{
          .run_uid = new_settings.run_gid.value_or(current_settings.run_uid),
          .run_gid = new_settings.run_gid.value_or(current_settings.run_gid),
          .controllers_override = new_settings.controllers_override.value_or(current_settings.controllers_override),
          .mouse_acceleration = new_settings.mouse_acceleration.value_or(current_settings.mouse_acceleration),
          .v_scroll_acceleration = new_settings.v_scroll_acceleration.value_or(current_settings.v_scroll_acceleration),
          .h_scroll_acceleration = new_settings.h_scroll_acceleration.value_or(current_settings.h_scroll_acceleration),
          .motion_controller_override =
              new_settings.motion_controller_override.value_or(current_settings.motion_controller_override),
      }};

  update_client_settings(this->state_->app_state->config, std::stoull(payload.client_id.value()), merged_client);

  auto res = GenericSuccessResponse{.success = true};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto icon_path = utils::split(req.query_string, '=');
  if (icon_path.size() != 2 || icon_path[0] != "icon_path") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'icon_path' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }
  // TODO: implement coroutines for CURL
  std::thread([this, socket, icon_path = utils::to_string(icon_path[1])]() {
    if (auto icon = utils::get_icon(this->state_->app_state->host->local_base_state_folder, icon_path)) {
      send_http(socket,
                200,
                {"Content-Length: " + std::to_string(icon->size()), "Content-Type: image/png"},
                icon.value());
    } else {
      auto res = GenericErrorResponse{.error = "Icon not found"};
      send_http(socket, 404, rfl::json::write(res));
    }
  }).detach();
}

void UnixSocketServer::endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto image_name = utils::split(req.query_string, '=');
  if (image_name.size() != 2 || image_name[0] != "image_name") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'image_name' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto response = docker_api.inspect_image(image_name[1])) {
    send_http(socket, 200, response.value());
  } else {
    auto res = GenericErrorResponse{.error = "Image not found"};
    send_http(socket, 404, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_payload = rfl::json::read<DockerPullImageRequest>(req.body);
  if (input_payload) {
    // TODO: implement coroutines for CURL
    std::thread([this, socket, image = input_payload.value().image_name]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      bool first_send = true;
      broadcast_event("DockerPullImageStartEvent",
                      rfl::json::write(events::DockerPullImageStartEvent{.image_name = image}));
      if (docker_api.pull_image(image,
                                {},
                                [this, &first_send, socket](const docker::DockerAPI::DockerProgressEvent &progress_ev) {
                                  if (first_send) {
                                    send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
                                    first_send = false;
                                  }
                                  auto serialized_ev = rfl::json::write(progress_ev) + "\r\n";
                                  send_data(socket, serialized_ev);
                                })) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        auto final_result = rfl::json::write(GenericSuccessResponse{.success = true});
        send_data(socket, final_result + "\r\n");
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = true}));
      } else {
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to pull image"}));
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = false}));
      }
    }).detach();
  }
}

} // namespace wolf::api