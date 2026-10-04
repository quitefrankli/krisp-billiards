#include "billiards_ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>


void BilliardsUiState::publish_power(const float value)
{
	const std::lock_guard lock(mutex);
	preview_power = std::clamp(value, 0.0f, 1.0f);
}

float BilliardsUiState::power() const
{
	const std::lock_guard lock(mutex);
	return preview_power;
}

void BilliardsUiState::request_reset()
{
	const std::lock_guard lock(mutex);
	reset_requested = true;
}

bool BilliardsUiState::take_reset_request()
{
	const std::lock_guard lock(mutex);
	return std::exchange(reset_requested, false);
}

void BilliardsUiState::publish_status(bool at_rest, std::size_t pocketed)
{
	const std::lock_guard lock(mutex);
	balls_at_rest = at_rest;
	pocketed_balls = pocketed;
}

BilliardsControlsWindow::BilliardsControlsWindow(BilliardsUiState& state) :
	ApplicationUiWindow({ "billiards_controls", "Billiards Physics Testbed" }),
	state(state)
{}

void BilliardsControlsWindow::draw_contents()
{
	ImGui::TextUnformatted("Physics backend: Jolt 5.2");
	{
		const std::lock_guard lock(state.mutex);
		ImGui::Text("Balls: %s | Pocketed: %zu", state.balls_at_rest ? "at rest" : "moving", state.pocketed_balls);
	}
	ImGui::Separator();
	ImGui::TextWrapped("Click the cue ball, pull the cue back, and release to shoot.");
	const float preview_power = state.power();
	ImGui::ProgressBar(preview_power, ImVec2(-1.0f, 0.0f));
	if (ImGui::Button("Reset Rack"))
		state.request_reset();
}
