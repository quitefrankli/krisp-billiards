#include "billiards_ui.hpp"

#include <camera.hpp>
#include <config.hpp>
#include <entity_component_system/light_source.hpp>
#include <game_engine.hpp>
#include <iapplication.hpp>
#include <renderable/material.hpp>
#include <renderable/mesh_factory.hpp>
#include <serialization/serializer.hpp>
#include <utility.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>


namespace
{
constexpr float TABLE_LENGTH = 10.0f;
constexpr float TABLE_WIDTH = 5.0f;
constexpr float TABLE_SURFACE_Y = 0.65f;
constexpr float BALL_RADIUS = 0.16f;
constexpr float BALL_Y = TABLE_SURFACE_Y + BALL_RADIUS;
constexpr float RAIL_HEIGHT = 0.32f;
constexpr float RAIL_THICKNESS = 0.32f;
constexpr float CORNER_GAP = 0.62f;
constexpr float SIDE_GAP = 0.72f;
constexpr float LONG_SEGMENT_LENGTH = (TABLE_LENGTH - 2.0f * CORNER_GAP - SIDE_GAP) * 0.5f;
constexpr float SHORT_RAIL_LENGTH = TABLE_WIDTH - 2.0f * CORNER_GAP;
constexpr float CUE_LENGTH = 3.0f;
constexpr float CUE_GAP = 0.08f;
constexpr float MAX_PULLBACK = 1.2f;
constexpr float DRAG_DISTANCE_FOR_MAX_POWER = 2.4f;
constexpr float MAX_SHOT_IMPULSE = 3.2f;
constexpr float ROLLING_RESISTANCE = 0.55f;
constexpr float ROLLING_STOP_SPEED = 0.04f;
constexpr glm::vec3 CUE_BALL_START{-2.5f, BALL_Y, 0.0f};
constexpr glm::vec3 RACK_APEX{2.0f, BALL_Y, 0.0f};

struct SpawnedObject
{
	ObjectID id;
	glm::vec3 initial_position;
	bool pocketed = false;
};

PbrMaterial make_material(const glm::vec3& color, const float roughness = 0.35f)
{
	return PbrMaterial(glm::vec4(color, 1.0f), 0.0f, roughness);
}

PbrMaterial make_felt_material()
{
	return make_material({ 0.03f, 0.34f, 0.16f }, 1.0f);
}

class BilliardsApplication : public IApplication
{
public:
	void create_ui(GameEngine&, ApplicationUiManager& ui) override
	{
		ui.set_theme({
			.text = { 0.94f, 0.96f, 0.92f, 1.0f },
			.window_background = { 0.04f, 0.10f, 0.07f, 0.94f },
			.accent = { 0.16f, 0.52f, 0.30f, 1.0f },
			.window_rounding = 8.0f,
			.window_border_size = 1.0f,
		});
		ui.register_window<BilliardsControlsWindow>({
			.anchor = ApplicationUiAnchor::TOP_RIGHT,
			.offset = { -16.0f, 16.0f },
			.size = { 330.0f, 175.0f },
		}, ui_state);
	}

	bool allows_playerless_normal_mode() const override { return true; }

	void on_begin(GameEngine& engine) override
	{
		this->engine = &engine;
		create_resources();
		spawn_table();
		spawn_balls();
		spawn_cue();
		spawn_light();

		engine.spawn_cubemap();
		set_fixed_camera(engine);
		engine.set_free_camera_movement(true);
		engine.set_camera_orbit_with_right_mouse(true);
		engine.set_normal_mode_cursor_captured(false);
		engine.set_game_mode(EGameMode::NORMAL);
		reset_rack();
	}

	void on_scene_loaded(GameEngine& engine) override
	{
		this->engine = &engine;
		balls_at_rest = std::ranges::all_of(balls, [&engine](const SpawnedObject& ball) {
			return !engine.get_ecs().is_body_enabled(ball.id) || !engine.get_ecs().is_body_active(ball.id);
		});
		set_cue_visible(false);
		charging = false;
		preview_power = 0.0f;
		aim_direction = Maths::right_vec;
		ui_state.publish_power(0.0f);
		ui_state.publish_status(balls_at_rest, pocketed_balls);
	}

	void serialize_scene(Serializer& out) const override
	{
		if (!felt_id || !table_id || !cue_id || rails.size() != 6 || balls.size() != 16)
			throw std::runtime_error("Billiards gameplay state is incomplete");
		out.write("felt_id", felt_id->get_underlying());
		out.write("table_id", table_id->get_underlying());
		out.write("cue_id", cue_id->get_underlying());
		auto saved_rails = out.sequence("rail_ids");
		for (const auto id : rails) saved_rails.append(id.get_underlying());
		auto saved_balls = out.sequence("balls");
		for (const auto& ball : balls) {
			auto saved = saved_balls.append_map();
			saved.write("id", ball.id.get_underlying());
			saved.write("initial_x", ball.initial_position.x);
			saved.write("initial_y", ball.initial_position.y);
			saved.write("initial_z", ball.initial_position.z);
			saved.write("pocketed", ball.pocketed);
		}
	}

	void deserialize_scene(GameEngine& engine, const Deserializer& in) override
	{
		this->engine = &engine;
		felt_id = required_id(in.read<std::uint64_t>("felt_id"));
		table_id = required_id(in.read<std::uint64_t>("table_id"));
		cue_id = required_id(in.read<std::uint64_t>("cue_id"));
		auto saved_rails = in.child("rail_ids").elements();
		if (saved_rails.size() != 6) throw SerializationError("Billiards save must contain six rails");
		rails.clear();
		for (const auto& saved : saved_rails) rails.push_back(required_id(saved.as<std::uint64_t>()));
		auto saved_balls = in.child("balls").elements();
		if (saved_balls.size() != 16) throw SerializationError("Billiards save must contain sixteen balls");
		balls.clear();
		pocketed_balls = 0;
		for (const auto& saved : saved_balls) {
			const auto id = required_id(saved.read<std::uint64_t>("id"));
			const glm::vec3 initial{
				saved.read<float>("initial_x"), saved.read<float>("initial_y"), saved.read<float>("initial_z") };
			if (!std::isfinite(initial.x) || !std::isfinite(initial.y) || !std::isfinite(initial.z))
				throw SerializationError("Billiards save contains an invalid ball reset position");
			const bool pocketed = saved.read<bool>("pocketed");
			if (!engine.get_ecs().has_rigid_body(id))
				throw SerializationError("Billiards save ball is missing its restored rigid body");
			if (pocketed && engine.get_ecs().is_body_enabled(id))
				throw SerializationError("Billiards save pocketed flag does not match its restored body state");
			if (std::ranges::any_of(balls, [id](const SpawnedObject& prior) { return prior.id == id; }))
				throw SerializationError("Billiards save contains duplicate ball IDs");
			balls.push_back({ id, initial, pocketed });
			if (pocketed) ++pocketed_balls;
		}
		std::unordered_set<ObjectID> gameplay_ids{*felt_id, *table_id, *cue_id};
		if (gameplay_ids.size() != 3)
			throw SerializationError("Billiards save contains duplicate gameplay object IDs");
		for (const auto id : rails)
			if (!gameplay_ids.insert(id).second)
				throw SerializationError("Billiards save contains duplicate gameplay object IDs");
		for (const auto& ball : balls)
			if (!gameplay_ids.insert(ball.id).second)
				throw SerializationError("Billiards save contains duplicate gameplay object IDs");
		for (const auto id : rails) {
			if (!engine.get_ecs().has_rigid_body(id))
				throw SerializationError("Billiards save rail is missing its restored rigid body");
		}
		if (!engine.get_ecs().has_rigid_body(*felt_id))
			throw SerializationError("Billiards save felt is missing its restored rigid body");
	}

	void on_tick(GameEngine& engine, float delta_secs) override
	{
		if (ui_state.take_reset_request())
			reset_rack();
		apply_rolling_resistance(engine, delta_secs);
		for (auto& ball : balls) {
			if (engine.get_ecs().is_body_enabled(ball.id)
				&& engine.get_ecs().get_position(ball.id).y < 0.0f) {
				engine.get_ecs().set_body_enabled(ball.id, false);
				if (!ball.pocketed) { ball.pocketed = true; ++pocketed_balls; }
			}
		}
		if (!engine.get_ecs().is_body_enabled(balls.front().id)
			&& std::ranges::all_of(balls.begin() + 1, balls.end(), [&engine](const SpawnedObject& ball) {
				return !engine.get_ecs().is_body_enabled(ball.id) || !engine.get_ecs().is_body_active(ball.id);
			})) {
			engine.get_ecs().set_body_enabled(balls.front().id, true);
			engine.get_ecs().teleport_body(balls.front().id, CUE_BALL_START);
			if (balls.front().pocketed && pocketed_balls > 0) --pocketed_balls;
			balls.front().pocketed = false;
		}
		balls_at_rest = std::ranges::all_of(balls, [&engine](const SpawnedObject& ball) {
			return !engine.get_ecs().is_body_enabled(ball.id) || !engine.get_ecs().is_body_active(ball.id);
		});
		ui_state.publish_status(balls_at_rest, pocketed_balls);

		if (!charging)
			return;

		const auto table_point = mouse_table_point(engine);
		if (!table_point)
			return;

		glm::vec3 drag = *table_point - charge_origin;
		drag.y = 0.0f;
		const float drag_distance = glm::length(drag);
		if (drag_distance > 0.001f)
			aim_direction = -drag / drag_distance;
		preview_power = std::clamp(drag_distance / DRAG_DISTANCE_FOR_MAX_POWER, 0.0f, 1.0f);
		ui_state.publish_power(preview_power);
		update_cue();
	}

	void on_mouse_button(GameEngine& engine, const MouseInput& input) override
	{
		if (input.button != EMouseButton::LEFT || input.modifier != EKeyModifier::NONE)
			return;

		if (input.action == EInputAction::RELEASE && charging)
		{
			if (preview_power > 0.0f)
				engine.get_ecs().add_impulse(balls.front().id, aim_direction * (preview_power * MAX_SHOT_IMPULSE));
			set_cue_visible(false);
			charging = false;
			preview_power = 0.0f;
			ui_state.publish_power(0.0f);
		}
	}

	void on_click(GameEngine& engine, Object& object) override
	{
		if (charging || object.get_id() != balls.front().id || !balls_at_rest
			|| !engine.get_ecs().is_body_enabled(balls.front().id))
			return;

		const auto table_point = mouse_table_point(engine);
		if (!table_point)
			return;

		charge_origin = *table_point;
		charging = true;
		preview_power = 0.0f;
		ui_state.publish_power(0.0f);
		update_cue();
		set_cue_visible(true);
	}
	void on_key_press(GameEngine&, const KeyInput&) override {}

private:
	void apply_rolling_resistance(GameEngine& engine, const float delta_secs)
	{
		for (const auto& ball : balls) {
			if (!engine.get_ecs().is_body_enabled(ball.id)) continue;
			const auto position = engine.get_ecs().get_position(ball.id);
			auto velocity = engine.get_ecs().get_linear_velocity(ball.id);
			if (std::abs(position.y - BALL_Y) > 0.02f || std::abs(velocity.y) > 0.1f) continue;

			const glm::vec2 horizontal{velocity.x, velocity.z};
			const float speed = glm::length(horizontal);
			if (speed == 0.0f) continue;
			const float speed_reduction = ROLLING_RESISTANCE * std::max(delta_secs, 0.0f);
			if (speed <= std::max(speed_reduction, ROLLING_STOP_SPEED)) {
				velocity.x = 0.0f;
				velocity.z = 0.0f;
				engine.get_ecs().set_linear_velocity(ball.id, velocity);
				engine.get_ecs().set_angular_velocity(ball.id, glm::vec3(0.0f));
				continue;
			}

			const float retained_speed = speed - speed_reduction;
			velocity.x *= retained_speed / speed;
			velocity.z *= retained_speed / speed;
			engine.get_ecs().set_linear_velocity(ball.id, velocity);
			engine.get_ecs().set_angular_velocity(ball.id,
				engine.get_ecs().get_angular_velocity(ball.id) * (retained_speed / speed));
		}
	}

	void create_resources()
	{
		auto& ecs = engine->get_ecs();
		cube_mesh = ecs.get_mesh_system().add(MeshFactory::cube());
		sphere_mesh = ecs.get_mesh_system().add(MeshFactory::sphere());
		circle_mesh = ecs.get_mesh_system().add(MeshFactory::circle({}, 48));
		cylinder_mesh = ecs.get_mesh_system().add(MeshFactory::cylinder({}, 24));

		felt_material = ecs.get_material_system().add(
			std::make_unique<PbrMaterial>(make_felt_material()));
		wood_material = ecs.get_material_system().add(
			std::make_unique<PbrMaterial>(make_material({ 0.24f, 0.09f, 0.035f })));
		pocket_material = ecs.get_material_system().add(
			std::make_unique<PbrMaterial>(make_material({ 0.008f, 0.008f, 0.01f }, 1.0f)));
		cue_material = ecs.get_material_system().add(
			std::make_unique<PbrMaterial>(make_material({ 0.72f, 0.52f, 0.25f })));
	}

	Object& spawn_primitive(
		const MeshHandle& mesh,
		const MaterialHandle& material,
		const glm::vec3& position,
		const glm::vec3& scale,
		const std::string_view name)
	{
		Renderable renderable;
		renderable.pipeline_render_type = ERenderType::COLOR;
		renderable.mesh_owner = mesh;
		renderable.material_owners = { material };
		auto& object = engine->spawn_object<Object>();
		object.set_name(name);
		engine->attach_renderable(object.get_id(), std::move(renderable));
		auto& transform = engine->get_ecs().get_transformation(object.get_id());
		transform.set_position(position);
		transform.set_scale(scale);
		return object;
	}

	void spawn_table()
	{
		table_id = spawn_primitive(cube_mesh, wood_material, { 0.0f, 0.2f, 0.0f },
			{ TABLE_LENGTH + 1.1f, 0.8f, TABLE_WIDTH + 1.1f }, "Table base").get_id();
		auto& felt = spawn_primitive(cube_mesh, felt_material, { 0.0f, 0.58f, 0.0f },
			{ TABLE_LENGTH, 0.14f, TABLE_WIDTH }, "Playing surface");
		felt_id = felt.get_id();
		add_static_box(felt.get_id(), { TABLE_LENGTH * 0.5f, 0.07f, TABLE_WIDTH * 0.5f });

		const float long_segment_offset = SIDE_GAP * 0.5f + LONG_SEGMENT_LENGTH * 0.5f;
		for (const float z : { -TABLE_WIDTH * 0.5f, TABLE_WIDTH * 0.5f })
		{
			for (const float x : { -long_segment_offset, long_segment_offset }) {
				auto& rail = spawn_primitive(cube_mesh, wood_material,
					{ x, TABLE_SURFACE_Y + RAIL_HEIGHT * 0.5f, z },
					{ LONG_SEGMENT_LENGTH, RAIL_HEIGHT, RAIL_THICKNESS }, "Long rail");
				rails.push_back(rail.get_id());
				add_static_box(rail.get_id(), {
					LONG_SEGMENT_LENGTH * 0.5f, RAIL_HEIGHT * 0.5f, RAIL_THICKNESS * 0.5f });
			}
		}
		for (const float x : { -TABLE_LENGTH * 0.5f, TABLE_LENGTH * 0.5f }) {
			auto& rail = spawn_primitive(cube_mesh, wood_material,
				{ x, TABLE_SURFACE_Y + RAIL_HEIGHT * 0.5f, 0.0f },
				{ RAIL_THICKNESS, RAIL_HEIGHT, SHORT_RAIL_LENGTH }, "Short rail");
			rails.push_back(rail.get_id());
			add_static_box(rail.get_id(), {
				RAIL_THICKNESS * 0.5f, RAIL_HEIGHT * 0.5f, SHORT_RAIL_LENGTH * 0.5f });
		}
		constexpr float pocket_radius = 0.34f;
		const std::array<glm::vec3, 6> pockets{{
			{ -TABLE_LENGTH * 0.5f, TABLE_SURFACE_Y + 0.005f, -TABLE_WIDTH * 0.5f },
			{ -TABLE_LENGTH * 0.5f, TABLE_SURFACE_Y + 0.005f,  TABLE_WIDTH * 0.5f },
			{ 0.0f, TABLE_SURFACE_Y + 0.005f, -TABLE_WIDTH * 0.5f },
			{ 0.0f, TABLE_SURFACE_Y + 0.005f,  TABLE_WIDTH * 0.5f },
			{ TABLE_LENGTH * 0.5f, TABLE_SURFACE_Y + 0.005f, -TABLE_WIDTH * 0.5f },
			{ TABLE_LENGTH * 0.5f, TABLE_SURFACE_Y + 0.005f,  TABLE_WIDTH * 0.5f },
		}};
		for (const auto& position : pockets)
			spawn_primitive(circle_mesh, pocket_material, position,
				glm::vec3(pocket_radius * 2.0f), "Pocket");
	}

	void add_static_box(ObjectID id, glm::vec3 half_extents)
	{
		engine->get_ecs().add_rigid_body(id, static_box_definition(half_extents));
	}

	static RigidBodyDefinition static_box_definition(glm::vec3 half_extents)
	{
		return RigidBodyDefinition{ .shape = BoxPhysicsShape{half_extents} };
	}

	void add_ball_body(ObjectID id, const bool enabled = true)
	{
		engine->get_ecs().add_rigid_body(id, RigidBodyDefinition{
			.shape = SpherePhysicsShape{BALL_RADIUS}, .motion = PhysicsMotionType::Dynamic,
			.quality = PhysicsMotionQuality::Continuous, .mass = 0.17f, .friction = 0.18f,
			.restitution = 0.92f, .linear_damping = 0.22f, .angular_damping = 0.12f,
			.enabled = enabled,
		});
		engine->get_ecs().set_contact_restitution(id, *felt_id, 0.05f);
	}

	ObjectID required_id(const std::uint64_t value) const
	{
		const ObjectID id(value);
		if (!engine->get_object(id))
			throw SerializationError("Billiards save refers to a missing object ID");
		return id;
	}

	static glm::vec3 rack_position(const int ball_index)
	{
		constexpr float row_spacing = BALL_RADIUS * 1.82f;
		constexpr float column_spacing = BALL_RADIUS * 2.08f;
		int index = 1;
		for (int row = 0; row < 5; ++row)
			for (int column = 0; column <= row; ++column, ++index)
				if (index == ball_index)
					return RACK_APEX + glm::vec3(
						row * row_spacing, 0.0f, (column - row * 0.5f) * column_spacing);
		throw std::out_of_range("Billiards rack ball index is out of range");
	}

	void spawn_balls()
	{
		const std::array<glm::vec3, 15> colors{{
			{ 0.95f, 0.78f, 0.08f }, { 0.12f, 0.28f, 0.85f }, { 0.82f, 0.12f, 0.10f },
			{ 0.38f, 0.12f, 0.58f }, { 0.015f, 0.015f, 0.018f }, { 0.08f, 0.52f, 0.20f },
			{ 0.55f, 0.08f, 0.08f }, { 0.95f, 0.42f, 0.06f }, { 0.92f, 0.72f, 0.12f },
			{ 0.18f, 0.38f, 0.92f }, { 0.92f, 0.18f, 0.15f }, { 0.48f, 0.18f, 0.68f },
			{ 0.98f, 0.50f, 0.10f }, { 0.12f, 0.62f, 0.26f }, { 0.68f, 0.12f, 0.12f },
		}};
		ball_materials.reserve(colors.size() + 1);
		ball_materials.push_back(engine->get_ecs().get_material_system().add(
			std::make_unique<PbrMaterial>(make_material(glm::vec3(0.96f), 0.15f))));
		for (const auto& color : colors)
			ball_materials.push_back(engine->get_ecs().get_material_system().add(
				std::make_unique<PbrMaterial>(make_material(color, 0.15f))));

		auto& cue_ball = spawn_primitive(sphere_mesh, ball_materials[0], CUE_BALL_START,
			glm::vec3(BALL_RADIUS * 2.0f), "Cue ball");
		balls.push_back({ cue_ball.get_id(), CUE_BALL_START });
		add_ball_body(cue_ball.get_id());
		engine->get_ecs().add_clickable_entity(cue_ball.get_id());

		int ball_index = 1;
		for (int row = 0; row < 5; ++row)
		{
			for (int column = 0; column <= row; ++column)
			{
				const glm::vec3 position = rack_position(ball_index);
				auto& ball = spawn_primitive(sphere_mesh, ball_materials[ball_index], position,
					glm::vec3(BALL_RADIUS * 2.0f), "Object ball " + std::to_string(ball_index));
				balls.push_back({ ball.get_id(), position });
				add_ball_body(ball.get_id());
				++ball_index;
			}
		}
	}

	void spawn_cue()
	{
		auto& cue = spawn_primitive(cylinder_mesh, cue_material, CUE_BALL_START,
			{ 0.055f, CUE_LENGTH, 0.055f }, "Cue stick");
		cue_id = cue.get_id();
	}

	void spawn_light()
	{
		auto light_material = engine->get_ecs().get_material_system().add(
			std::make_unique<PbrMaterial>(make_material({ 1.0f, 0.92f, 0.76f })));
		Renderable marker{
			.pipeline_render_type = ERenderType::COLOR,
			.shading_mode = EShadingMode::UNLIT,
			.casts_shadow = false,
			.mesh_owner = sphere_mesh,
			.material_owners = { light_material },
		};
		auto& light = engine->spawn_object<Object>();
		light.set_name("Overhead light");
		engine->attach_renderable(light.get_id(), std::move(marker));
		auto& transform = engine->get_ecs().get_transformation(light.get_id());
		transform.set_position({ -1.0f, 7.0f, -1.0f });
		transform.set_scale(glm::vec3(0.24f));
		engine->get_ecs().add_light_source(light.get_id(), {
			.intensity = 225.0f,
			.color = { 1.0f, 0.92f, 0.78f },
		});
	}

	void reset_rack()
	{
		for (auto& ball : balls)
		{
			engine->get_ecs().set_body_enabled(ball.id, true);
			engine->get_ecs().teleport_body(ball.id, ball.initial_position);
			ball.pocketed = false;
		}
		pocketed_balls = 0;
		balls_at_rest = true;
		set_cue_visible(false);
		charging = false;
		preview_power = 0.0f;
		aim_direction = Maths::right_vec;
		ui_state.publish_power(0.0f);
		update_cue();
	}

	void update_cue()
	{
		if (!cue_id)
			return;
		const float pullback = preview_power * MAX_PULLBACK;
		const float distance = BALL_RADIUS + CUE_GAP + CUE_LENGTH * 0.5f + pullback;
		auto& transform = engine->get_ecs().get_transformation(*cue_id);
		transform.set_position(engine->get_ecs().get_position(balls.front().id) - aim_direction * distance);
		transform.set_rotation(Maths::RotationBetweenVectors(Maths::up_vec, aim_direction));
	}

	void set_cue_visible(const bool visible)
	{
		if (cue_id)
			engine->get_object(*cue_id)->set_visibility(visible);
	}

	static std::optional<glm::vec3> mouse_table_point(const GameEngine& engine)
	{
		const Maths::Ray ray = engine.get_mouse_ray();
		const Maths::Plane table_plane({ 0.0f, TABLE_SURFACE_Y, 0.0f }, Maths::up_vec);
		if (!Maths::check_ray_plane_intersection(ray, table_plane))
			return std::nullopt;
		return Maths::ray_plane_intersection(ray, table_plane);
	}

	static void set_fixed_camera(GameEngine& engine)
	{
		engine.get_camera().look_at(
			glm::vec3(0.0f, TABLE_SURFACE_Y, 0.0f),
			glm::vec3(-7.5f, 8.5f, -8.5f));
	}

	GameEngine* engine = nullptr;
	BilliardsUiState ui_state;
	MeshHandle cube_mesh;
	MeshHandle sphere_mesh;
	MeshHandle circle_mesh;
	MeshHandle cylinder_mesh;
	MaterialHandle felt_material;
	MaterialHandle wood_material;
	MaterialHandle pocket_material;
	MaterialHandle cue_material;
	std::vector<MaterialHandle> ball_materials;
	std::vector<SpawnedObject> balls;
	std::optional<ObjectID> felt_id;
	std::optional<ObjectID> table_id;
	std::optional<ObjectID> cue_id;
	std::vector<ObjectID> rails;
	glm::vec3 aim_direction = Maths::right_vec;
	glm::vec3 charge_origin{};
	float preview_power = 0.0f;
	bool charging = false;
	bool balls_at_rest = true;
	std::size_t pocketed_balls = 0;
};
}

int main(int, char**)
{
	auto runtime_paths = Utility::paths_for_executable("billiards");
	runtime_paths.app_resources = runtime_paths.app_resources.parent_path();
	Config::init("billiards", std::move(runtime_paths));
	auto engine = GameEngine::create<BilliardsApplication>();
	engine.run();
}
