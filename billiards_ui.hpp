#pragma once

#include <gui/application_ui_manager.hpp>

#include <mutex>


class BilliardsUiState
{
public:
	void publish_power(float value);
	float power() const;
	void request_reset();
	bool take_reset_request();
	void publish_status(bool at_rest, std::size_t pocketed);

private:
	friend class BilliardsControlsWindow;
	mutable std::mutex mutex;
	float preview_power = 0.0f;
	bool reset_requested = false;
	bool balls_at_rest = true;
	std::size_t pocketed_balls = 0;
};

class BilliardsControlsWindow : public ApplicationUiWindow
{
public:
	explicit BilliardsControlsWindow(BilliardsUiState& state);

private:
	void draw_contents() override;

	BilliardsUiState& state;
};
