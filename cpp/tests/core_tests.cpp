#include "whitehole/app/settings.hpp"
#include "whitehole/app/theme_palette.hpp"
#include "whitehole/db/data_holder.hpp"
#include "whitehole/db/name_table.hpp"
#include "whitehole/db/hints.hpp"
#include "whitehole/db/areamanagerlimits.hpp"
#include "whitehole/db/shortcuts.hpp"
#include "whitehole/db/modelsubstitutions.hpp"
#include "whitehole/db/specialrenderers.hpp"
#include "whitehole/edit/document.hpp"
#include "whitehole/edit/validation.hpp"
#include "whitehole/db/object_db.hpp"
#include "whitehole/edit/authoring.hpp"
#include "whitehole/edit/commands.hpp"
#include "whitehole/edit/undo.hpp"
#include "whitehole/io/binary_file.hpp"
#include "whitehole/io/directory_filesystem.hpp"
#include "whitehole/io/rarc.hpp"
#include "whitehole/io/yaz0.hpp"
#include "whitehole/util/json.hpp"
#include "whitehole/util/text.hpp"
#include "whitehole/math/geometry.hpp"
#include "whitehole/render/camera.hpp"
#include "whitehole/render/gizmo.hpp"
#include "whitehole/render/model_mesh.hpp"
#include "whitehole/render/object_visual.hpp"
#include "whitehole/render/viewport_scene.hpp"
#include "whitehole/smg/bcsv.hpp"
#include "whitehole/smg/bmd.hpp"
#include "whitehole/smg/bti.hpp"
#include "whitehole/smg/game_archive.hpp"
#include "whitehole/smg/hash.hpp"
#include "whitehole/smg/object_model.hpp"
#include "whitehole/smg/path.hpp"
#include "whitehole/smg/stage_archive.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testBinaryData() {
    whitehole::io::BinaryWriter writer(whitehole::io::Endian::big);
    writer.writeU8(0x12);
    writer.writeU16(0x3456);
    writer.writeU32(0x789ABCDE);
    writer.writeF32(1.25F);
    writer.writeString("Galaxy");

    whitehole::io::BinaryReader reader(writer.data(), whitehole::io::Endian::big);
    expect(reader.readU8() == 0x12, "8-bit binary round trip failed");
    expect(reader.readU16() == 0x3456, "16-bit binary round trip failed");
    expect(reader.readU32() == 0x789ABCDE, "32-bit binary round trip failed");
    expect(std::abs(reader.readF32() - 1.25F) < 0.00001F, "float binary round trip failed");
    expect(reader.readString() == "Galaxy", "string binary round trip failed");

    bool rejected = false;
    try {
        (void)reader.readU8();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "out-of-bounds binary read was not rejected");
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto unique = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() / ("whitehole-native-tests-" + unique);
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("could not create temporary test directory");
        }
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path path;
};

void testDirectoryFilesystem() {
    TemporaryDirectory temporary;
    whitehole::io::DirectoryFilesystem project(temporary.path);
    project.createDirectory("/StageData/TestGalaxy");
    project.write("/StageData/TestGalaxy/TestGalaxyMap.arc", {1, 2, 3});
    expect(project.directoryExists("/StageData/TestGalaxy"), "project directory was not created");
    expect(project.fileExists("/StageData/TestGalaxy/TestGalaxyMap.arc"), "project file was not created");
    expect(project.read("/StageData/TestGalaxy/TestGalaxyMap.arc") == std::vector<std::uint8_t>({1, 2, 3}),
           "project file contents changed");
    expect(project.directories("/StageData") == std::vector<std::string>({"TestGalaxy"}),
           "project directory listing changed");
    expect(project.files("/StageData/TestGalaxy") == std::vector<std::string>({"TestGalaxyMap.arc"}),
           "project file listing changed");

    project.renameFile("/StageData/TestGalaxy/TestGalaxyMap.arc", "Renamed.arc");
    expect(project.fileExists("/StageData/TestGalaxy/Renamed.arc"), "project file rename failed");
    project.renameDirectory("/StageData/TestGalaxy", "RenamedGalaxy");
    expect(project.fileExists("/StageData/RenamedGalaxy/Renamed.arc"), "project directory rename failed");

    bool traversalRejected = false;
    try {
        (void)project.read("/../outside");
    } catch (const std::runtime_error&) {
        traversalRejected = true;
    }
    expect(traversalRejected, "project path traversal was not rejected");

    project.removeFile("/StageData/RenamedGalaxy/Renamed.arc");
    project.removeDirectory("/StageData/RenamedGalaxy");
    expect(!project.directoryExists("/StageData/RenamedGalaxy"), "project directory removal failed");
}

void testYaz0() {
    std::vector<std::uint8_t> input;
    const std::string pattern = "Whitehole Pro native archive support! ";
    for (int index = 0; index < 100; ++index) {
        input.insert(input.end(), pattern.begin(), pattern.end());
        input.push_back(static_cast<std::uint8_t>(index));
    }
    const auto compressed = whitehole::io::yaz0::compress(input);
    expect(whitehole::io::yaz0::isCompressed(compressed), "Yaz0 output has no header");
    expect(compressed.size() < input.size(), "Yaz0 did not compress repetitive data");
    expect(whitehole::io::yaz0::decompress(compressed) == input, "Yaz0 round trip failed");
    expect(whitehole::io::yaz0::compress(compressed) == compressed, "Yaz0 double compression changed data");
    expect(whitehole::io::yaz0::decompress(whitehole::io::yaz0::compress(std::vector<std::uint8_t>{})).empty(),
           "empty Yaz0 round trip failed");

    bool rejected = false;
    try {
        (void)whitehole::io::yaz0::decompress({'Y', 'a', 'z', '0'});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "truncated Yaz0 data was not rejected");
}

std::vector<std::uint8_t> makeTinyRarc(whitehole::io::Endian endian) {
    whitehole::io::BinaryWriter writer(endian);
    writer.writeU32(0x52415243); // RARC
    writer.writeU32(0xA3);      // file size
    writer.writeU32(0x20);      // header size
    writer.writeU32(0x80);      // data offset, relative to 0x20
    writer.writeU32(3);
    writer.writeU32(3);
    writer.writeU32(0);
    writer.writeU32(0);
    writer.writeU32(1);         // node count
    writer.writeU32(0x20);      // node table at 0x40
    writer.writeU32(3);         // entry count
    writer.writeU32(0x30);      // entry table at 0x50
    writer.writeU32(16);        // string table length
    writer.writeU32(0x70);      // string table at 0x90
    writer.writeU32(0);
    writer.writeU32(0);

    writer.seek(0x40);
    writer.writeU32(0x524F4F54); // ROOT
    writer.writeU32(5);          // "root"
    writer.writeU16(0);
    writer.writeU16(3);
    writer.writeU32(0);

    const auto writeDirectoryEntry = [&](std::uint16_t nameOffset, std::uint32_t node) {
        writer.writeU16(0xFFFF);
        writer.writeU16(0);
        if (endian == whitehole::io::Endian::big) {
            writer.writeU16(0x0200);
            writer.writeU16(nameOffset);
        } else {
            writer.writeU16(nameOffset);
            writer.writeU16(0x0200);
        }
        writer.writeU32(node);
        writer.writeU32(0x10);
        writer.writeU32(0);
    };
    writeDirectoryEntry(0, 0);
    writeDirectoryEntry(2, 0xFFFFFFFF);

    writer.writeU16(0);
    writer.writeU16(0);
    if (endian == whitehole::io::Endian::big) {
        writer.writeU16(0x1100);
        writer.writeU16(10);
    } else {
        writer.writeU16(10);
        writer.writeU16(0x1100);
    }
    writer.writeU32(0);
    writer.writeU32(3);
    writer.writeU32(0);

    writer.seek(0x90);
    writer.writeString(".");
    writer.writeString("..");
    writer.writeString("root");
    writer.writeString("file");
    writer.seek(0xA0);
    writer.writeU8(1);
    writer.writeU8(2);
    writer.writeU8(3);
    return std::move(writer).take();
}

void testRarcEndianness() {
    for (const auto endian : {whitehole::io::Endian::big, whitehole::io::Endian::little}) {
        whitehole::io::RarcArchive archive(makeTinyRarc(endian));
        expect(archive.endian() == endian, "RARC endian detection failed");
        expect(archive.rootName() == "root", "RARC root name changed");
        expect(archive.entries().size() == 1, "RARC entry table was not parsed");
        expect(archive.entries().front().path == "root/file", "RARC entry path changed");
        expect(archive.read(archive.entries().front()) == std::vector<std::uint8_t>({1, 2, 3}),
               "RARC entry contents changed");
        archive.replace("ROOT/FILE", {9, 8, 7, 6});
        const whitehole::io::RarcArchive rewritten(archive.serialize(false));
        expect(rewritten.endian() == endian, "rewritten RARC endian changed");
        expect(rewritten.entries().size() == 1, "rewritten RARC entry count changed");
        expect(rewritten.read(rewritten.entries().front()) == std::vector<std::uint8_t>({9, 8, 7, 6}),
               "RARC replacement was not serialized");
    }
}

void testMath() {
    using whitehole::math::Matrix4;
    using whitehole::math::Vec3f;
    const auto transformed = Matrix4::translation({10, 20, 30}).transformPoint({1, 2, 3});
    expect(std::abs(transformed.x - 11) < 0.00001F, "matrix X translation failed");
    expect(std::abs(transformed.y - 22) < 0.00001F, "matrix Y translation failed");
    expect(std::abs(transformed.z - 33) < 0.00001F, "matrix Z translation failed");
    const auto composed = Matrix4::translation({10, 20, 30}) * Matrix4::scale({2, 3, 4});
    const auto composedPoint = composed.transformPoint({1, 2, 3});
    expect(std::abs(composedPoint.x - 12) < 0.00001F, "matrix composition scaled X translation");
    expect(std::abs(composedPoint.y - 26) < 0.00001F, "matrix composition scaled Y translation");
    expect(std::abs(composedPoint.z - 42) < 0.00001F, "matrix composition scaled Z translation");
    expect(std::abs(Vec3f::dot({1, 0, 0}, {0, 1, 0})) < 0.00001F, "vector dot product failed");
    const auto cross = Vec3f::cross({1, 0, 0}, {0, 1, 0});
    expect(std::abs(cross.z - 1) < 0.00001F, "vector cross product failed");
}

void testViewportCamera() {
    whitehole::render::ViewportCamera camera;
    camera.target = {100.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;

    const auto eye = camera.eye();
    expect(std::abs(eye.x - 600.0F) < 0.01F, "viewport camera eye is wrong");

    // Center of the screen must ray-cast straight at the orbit target.
    const auto ray = camera.screenToRay(400.0F, 300.0F, 800.0F, 600.0F);
    const auto toTarget = whitehole::math::Vec3f{100.0F - ray.origin.x, 0.0F - ray.origin.y, 0.0F - ray.origin.z};
    const float alignment = whitehole::math::Vec3f::dot(ray.direction, toTarget.normalized());
    expect(alignment > 0.999F, "viewport camera center ray misses target");

    // Round trip through world->screen keeps the target centered.
    float screenX = 0.0F;
    float screenY = 0.0F;
    expect(camera.worldToScreen(camera.target, 800.0F, 600.0F, screenX, screenY), "target behind viewport camera");
    expect(std::abs(screenX - 400.0F) < 1.0F && std::abs(screenY - 300.0F) < 1.0F,
           "viewport camera projection is off-center");

    const float before = camera.distance;
    camera.dolly(1.0F);
    expect(camera.distance < before, "viewport camera dolly-in failed");
    camera.frameTarget({1.0F, 2.0F, 3.0F}, 250.0F);
    expect(std::abs(camera.target.x - 1.0F) < 0.001F && std::abs(camera.distance - 250.0F) < 0.001F,
           "viewport camera framing failed");

    // Zoom and framing reach galaxy-spanning distances: the old hard 20000
    // clamp cut big maps off mid-view and made Frame All impossible. dolly()
    // clamps one gesture to +/-4 notches, so a full zoom-out is several flicks.
    camera.distance = 1000.0F;
    for (int flick = 0; flick < 12; ++flick) {
        camera.dolly(-4.0F);
    }
    expect(camera.distance > 20000.0F, "dolly-out is stuck below the old clamp");
    expect(camera.distance <= 150001.0F, "dolly-out exceeded the new ceiling");
    camera.frameTarget({}, 250000.0F);
    expect(camera.distance > 20000.0F, "frameTarget clamps below galaxy size");
    expect(camera.distance <= 150001.0F, "frameTarget exceeded the new ceiling");

    // Dynamic clip planes stay inside their band and ordered at every zoom --
    // this is what killed the far-range z-fighting on rails and overlays.
    for (const float zoom : {5.0F, 800.0F, 40000.0F, 150000.0F}) {
        camera.distance = zoom;
        const float near = camera.nearPlane();
        const float far = camera.farPlane(50000.0F);
        expect(near >= 1.0F && near <= 100.0F, "near plane left its band");
        expect(far > near, "far plane must sit beyond near");
        expect(far <= 500001.0F, "far plane exceeded its cap");
    }

    // Fly slides the orbit target along the camera basis. With yaw/pitch at 0,
    // target (100,0,0) and distance 500 the eye sits at +X, so forward is -X
    // and right is -Z.
    camera.target = {100.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    camera.fly(0.0F, 0.0F, 100.0F);
    expect(std::abs(camera.target.x - 0.0F) < 0.01F, "fly forward moved the wrong way");
    camera.fly(100.0F, 0.0F, 0.0F);
    expect(std::abs(camera.target.z - (-100.0F)) < 0.01F, "fly right moved the wrong way");
    camera.fly(0.0F, 50.0F, 0.0F);
    expect(std::abs(camera.target.y - 50.0F) < 0.01F, "fly up moved the wrong way");

    // The grid covers the visible ground (half-extent >= orbit distance, so
    // the full patch spans >= 2x) and snaps to clean 1/2/5 decade steps.
    for (const float zoom : {5.0F, 40.0F, 800.0F, 30000.0F}) {
        const auto grid = whitehole::render::gridSpec(zoom);
        expect(grid.extent >= zoom * 0.999F, "grid does not cover the visible ground");
        const float decade = std::pow(10.0F, std::floor(std::log10(grid.step)));
        const float mantissa = grid.step / decade;
        expect(std::abs(mantissa - 1.0F) < 0.001F || std::abs(mantissa - 2.0F) < 0.001F ||
                   std::abs(mantissa - 5.0F) < 0.001F,
               "grid step is not a 1/2/5 decade value");
    }
}

void testGizmoMath() {
    using whitehole::render::axisDirection;
    using whitehole::render::beginGizmoDrag;
    using whitehole::render::GizmoDrag;
    using whitehole::render::GizmoHandle;
    using whitehole::render::GizmoMode;
    using whitehole::render::gizmoAxisLength;
    using whitehole::render::gizmoDragValue;
    using whitehole::render::pickGizmoHandle;

    // An isometric view where every axis is visibly distinct.
    whitehole::render::ViewportCamera camera;
    camera.target = {0.0F, 0.0F, 0.0F};
    camera.yawRadians = 0.7853982F;
    camera.pitchRadians = 0.6F;
    camera.distance = 1000.0F;
    constexpr float kWidth = 800.0F;
    constexpr float kHeight = 600.0F;

    expect(axisDirection(GizmoHandle::AxisX).x == 1.0F, "axis X direction wrong");
    expect(axisDirection(GizmoHandle::AxisY).y == 1.0F, "axis Y direction wrong");
    expect(axisDirection(GizmoHandle::AxisZ).z == 1.0F, "axis Z direction wrong");

    // Constant-screen-size sizing: the axis must read ~80 px whatever the zoom.
    const float axisLength = gizmoAxisLength(camera, {0.0F, 0.0F, 0.0F}, kHeight);
    float centreX = 0.0F;
    float centreY = 0.0F;
    expect(camera.worldToScreen({0.0F, 0.0F, 0.0F}, kWidth, kHeight, centreX, centreY),
           "the gizmo anchor must project");
    float tipX = 0.0F;
    float tipY = 0.0F;
    expect(camera.worldToScreen({axisLength, 0.0F, 0.0F}, kWidth, kHeight, tipX, tipY),
           "the gizmo tip must project");
    const float screenLength = std::hypot(tipX - centreX, tipY - centreY);
    expect(screenLength > 60.0F && screenLength < 110.0F,
           "the gizmo axis must stay a constant on-screen size");

    // Clicking on each axis tip grabs that axis.
    for (const auto handle : {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ}) {
        const auto direction = axisDirection(handle);
        float grabX = 0.0F;
        float grabY = 0.0F;
        expect(camera.worldToScreen(
                   {direction.x * axisLength, direction.y * axisLength, direction.z * axisLength},
                   kWidth, kHeight, grabX, grabY),
               "an axis tip must project");
        const auto picked =
            pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, grabX, grabY, kWidth, kHeight);
        expect(picked == handle, "picking missed the axis it clicked on");
    }

    // Empty space grabs nothing; the anchor grabs the centre.
    expect(pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, 40.0F, 40.0F, kWidth, kHeight) ==
               GizmoHandle::None,
           "picking must miss empty space");
    expect(pickGizmoHandle(camera, {0.0F, 0.0F, 0.0F}, centreX, centreY, kWidth, kHeight) ==
               GizmoHandle::Center,
           "picking must grab the centre at the anchor");

    // ---- translate drag --------------------------------------------------
    GizmoDrag drag;
    expect(beginGizmoDrag(drag, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Translate,
                          GizmoHandle::AxisX, tipX, tipY, kWidth, kHeight),
           "beginGizmoDrag refused the X axis");
    // Dragging *along* the handle's screen direction moves only X, positively,
    // proportional to the pixel travel.
    constexpr float kTravelPx = 30.0F;
    const auto moved = gizmoDragValue(drag, camera, tipX + drag.axisDir.x * kTravelPx,
                                      tipY + drag.axisDir.y * kTravelPx, kWidth, kHeight);
    expect(std::abs(moved.y) < 0.5F && std::abs(moved.z) < 0.5F,
           "an axis drag must not leak into the other axes");
    expect(moved.x > 5.0F, "a 30px drag along X must move the object visibly");
    expect(std::abs(moved.x - kTravelPx * drag.worldPerPixel) < 0.5F, "axis drag mapping is off");

    // Dragging back to the grab point reads exactly zero.
    const auto rest = gizmoDragValue(drag, camera, tipX, tipY, kWidth, kHeight);
    expect(std::abs(rest.x) < 0.01F && std::abs(rest.y) < 0.01F && std::abs(rest.z) < 0.01F,
           "a drag returned to its start must read zero");

    // An axis head-on to the camera projects to a point and cannot be picked:
    // begin refuses instead of inventing a mapping the author cannot see.
    whitehole::render::ViewportCamera headOn;
    headOn.target = {0.0F, 0.0F, 0.0F};
    headOn.yawRadians = 0.0F; // eye on +X looking down -X: the X axis is head-on
    headOn.pitchRadians = 0.0F;
    headOn.distance = 1000.0F;
    float headCentreX = 0.0F;
    float headCentreY = 0.0F;
    expect(headOn.worldToScreen({0.0F, 0.0F, 0.0F}, kWidth, kHeight, headCentreX, headCentreY),
           "the head-on anchor must project");
    expect(pickGizmoHandle(headOn, {0.0F, 0.0F, 0.0F}, headCentreX, headCentreY, kWidth,
                           kHeight) != GizmoHandle::AxisX,
           "a view-parallel axis must not be grabbable");

    // ---- centre drag -----------------------------------------------------
    // The centre moves in the view plane through the anchor.
    GizmoDrag centre;
    expect(beginGizmoDrag(centre, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Translate,
                          GizmoHandle::Center, centreX, centreY, kWidth, kHeight),
           "beginGizmoDrag refused the centre");
    const auto planar = gizmoDragValue(centre, camera, centreX + 40.0F, centreY, kWidth, kHeight);
    const float planarLength = planar.length();
    expect(planarLength > 5.0F, "a 40px centre drag must move the object visibly");
    const auto forward = camera.forward();
    expect(std::abs(whitehole::math::Vec3f::dot(planar, forward)) < planarLength * 0.02F,
           "a centre drag must stay in the view plane");

    // ---- scale drag ------------------------------------------------------
    // One axis length of drag must double the scale on that axis alone.
    GizmoDrag scale;
    // Rebuilt from the Y axis tip, which exercises a different axis than the
    // translate test above.
    {
        const auto direction = axisDirection(GizmoHandle::AxisY);
        float grabX = 0.0F;
        float grabY = 0.0F;
        expect(camera.worldToScreen({direction.x * axisLength, direction.y * axisLength,
                                     direction.z * axisLength},
                                    kWidth, kHeight, grabX, grabY),
               "the Y axis tip must project");
        expect(beginGizmoDrag(scale, camera, {0.0F, 0.0F, 0.0F}, GizmoMode::Scale,
                              GizmoHandle::AxisY, grabX, grabY, kWidth, kHeight),
               "beginGizmoDrag refused the Y tip");
        const auto doubled =
            gizmoDragValue(scale, camera, grabX + scale.axisDir.x * screenLength,
                           grabY + scale.axisDir.y * screenLength, kWidth, kHeight);
        expect(std::abs(doubled.y - 2.0F) < 0.05F, "an axis-length drag must double the scale");
        expect(std::abs(doubled.x - 1.0F) < 0.01F && std::abs(doubled.z - 1.0F) < 0.01F,
               "a scale drag must not touch the other axes");
    }
}

void testViewportScene() {
    whitehole::smg::PlacementObject object;
    object.name = "Kinopio";
    object.kind = "obj";
    object.position = {100.0F, 0.0F, 0.0F};
    object.rotation = {0.0F, 0.0F, 0.0F};
    object.scale = {1.0F, 1.0F, 1.0F};

    whitehole::render::ViewportScene scene;
    scene.rebuild({object});
    expect(scene.boxes().size() == 1, "viewport scene dropped the object");
    const auto& box = scene.boxes().front();
    expect(std::abs(box.center.x - 100.0F) < 0.001F, "viewport box center is wrong");

    // Box matrix must map the unit-box corner onto position + half extent.
    const auto corner = box.world.transformPoint({1.0F, 1.0F, 1.0F});
    expect(std::abs(corner.x - 125.0F) < 0.01F, "viewport box world matrix is wrong");

    whitehole::render::ViewportCamera camera;
    camera.target = object.position;
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    const auto hit = scene.pick(camera, 400.0F, 300.0F, 800.0F, 600.0F);
    expect(hit.has_value() && *hit == 0, "viewport picking missed the centered object");
    // Far corner of the screen should miss the single centered box.
    expect(!scene.pick(camera, 799.0F, 599.0F, 800.0F, 600.0F).has_value(), "viewport picking hit empty space");
}

void testObjectVisual() {
    using whitehole::render::ObjectCategory;
    using whitehole::render::categoryStyle;
    using whitehole::render::classifyObject;
    using whitehole::render::objectStyle;
    using whitehole::render::shapeTriangles;

    // Table kind drives the category first.
    expect(classifyObject("start", "Mario") == ObjectCategory::Player, "start objects should classify as Player");
    expect(classifyObject("camera", "CameraPos") == ObjectCategory::Camera, "camera table should classify as Camera");
    expect(classifyObject("area", "AreaVolume") == ObjectCategory::Zone, "area table should classify as Zone");
    expect(classifyObject("gravity", "GravitySphere") == ObjectCategory::Gravity, "gravity table should classify as Gravity");
    expect(classifyObject("mappart", "Elevator") == ObjectCategory::MapPart, "mappart table should classify as MapPart");
    expect(classifyObject("cutscene", "Demo") == ObjectCategory::Cutscene, "cutscene table should classify as Cutscene");

    // Object names refine plain "obj" placements.
    expect(classifyObject("obj", "Kinopio") == ObjectCategory::Player, "Kinopio should classify as Player");
    expect(classifyObject("obj", "Kuribo") == ObjectCategory::Enemy, "Kuribo should classify as Enemy");
    expect(classifyObject("obj", "BossKuriboJunior") == ObjectCategory::Enemy, "Boss names should classify as Enemy");
    expect(classifyObject("obj", "PowerStar") == ObjectCategory::Item, "PowerStar should classify as Item");
    expect(classifyObject("obj", "PurpleCoin") == ObjectCategory::Item, "coins should classify as Item");
    expect(classifyObject("obj", "PlanetDifferencesAndBeyond") == ObjectCategory::Terrain, "planets should classify as Terrain");
    expect(classifyObject("obj", "OceanWave") == ObjectCategory::Misc, "unknown names should classify as Misc");

    // Every category has a distinct, valid color (the whole visual system
    // depends on categories being tellable apart at a glance).
    for (std::size_t a = 0; a < whitehole::render::categoryCount(); ++a) {
        const auto& styleA = categoryStyle(static_cast<ObjectCategory>(a));
        expect(std::abs(styleA.color[0]) + std::abs(styleA.color[1]) + std::abs(styleA.color[2]) > 0.1F,
               "category color must not be black");
        for (std::size_t b = a + 1; b < whitehole::render::categoryCount(); ++b) {
            const auto& styleB = categoryStyle(static_cast<ObjectCategory>(b));
            const bool same = std::abs(styleA.color[0] - styleB.color[0]) < 0.01F &&
                              std::abs(styleA.color[1] - styleB.color[1]) < 0.01F &&
                              std::abs(styleA.color[2] - styleB.color[2]) < 0.01F;
            expect(!same, "category colors must be distinct");
            expect(styleA.shape != styleB.shape || std::string_view(styleA.label) != std::string_view(styleB.label),
                   "categories must differ in shape or label");
        }
    }

    // Shape meshes are unit-sized triangle soup.
    const auto cube = shapeTriangles(whitehole::render::CategoryStyle::Shape::Cube);
    expect(cube.size() == 12 * 3, "cube mesh should have 12 triangles");
    const auto sphere = shapeTriangles(whitehole::render::CategoryStyle::Shape::Sphere);
    expect(sphere.size() == 12 * 8 * 2 * 3, "sphere mesh triangle count is wrong");
    const auto pyramid = shapeTriangles(whitehole::render::CategoryStyle::Shape::Pyramid);
    expect(pyramid.size() == 6 * 3, "pyramid mesh should have 6 triangles");
    const auto octa = shapeTriangles(whitehole::render::CategoryStyle::Shape::Octahedron);
    expect(octa.size() == 8 * 3, "octahedron mesh should have 8 triangles");
    const auto cylinder = shapeTriangles(whitehole::render::CategoryStyle::Shape::Cylinder);
    expect(cylinder.size() == 12 * 4 * 3, "cylinder mesh triangle count is wrong");

    const float maxComponent = [](const std::vector<whitehole::math::Vec3f>& triangles) {
        float worst = 0.0F;
        for (const auto& vertex : triangles) {
            worst = std::max(worst, std::max({std::abs(vertex.x), std::abs(vertex.y), std::abs(vertex.z)}));
        }
        return worst;
    }(cube);
    expect(maxComponent < 1.0001F, "cube mesh must stay within the unit cube");

    // The scene stores the category so renderers and lists can share it.
    whitehole::smg::PlacementObject object;
    object.name = "Kuribo";
    object.kind = "obj";
    object.scale = {0.001F, 0.001F, 0.001F};
    whitehole::render::ViewportScene scene;
    scene.rebuild({object});
    expect(scene.boxes().size() == 1, "viewport scene dropped the object");
    expect(scene.boxes().front().category == ObjectCategory::Enemy, "viewport scene lost the object category");
    // Micro-scaled objects are clamped to the minimum visual scale so they
    // stay visible and clickable.
    expect(std::abs(scene.boxes().front().halfExtents.x - 25.0F * whitehole::render::kMinVisualScale) < 0.01F,
           "minimum visual scale clamp failed");
    // objectStyle ties classification to visuals in one call.
    expect(&objectStyle("obj", "PowerStar") == &categoryStyle(ObjectCategory::Item),
           "objectStyle should match classifyObject");
}

void testHashes() {
    expect(whitehole::smg::jmapHash("name") == 0x00337A8B, "JMap hash does not match the game algorithm");
    expect(whitehole::smg::superFastHash("") == 0, "empty SuperFastHash changed");
    expect(whitehole::smg::superFastHash("Whitehole") == 0x680E328E, "SuperFastHash compatibility vector changed");
}

std::vector<std::uint8_t> makeTinyBcsv(whitehole::io::Endian endian) {
    whitehole::io::BinaryWriter writer(endian);
    writer.writeU32(1);    // rows
    writer.writeU32(2);    // fields
    writer.writeU32(0x28); // data offset
    writer.writeU32(8);    // row size

    writer.writeU32(whitehole::smg::jmapHash("number"));
    writer.writeU32(0x0000FFFF);
    writer.writeU16(0);
    writer.writeU8(0);
    writer.writeU8(static_cast<std::uint8_t>(whitehole::smg::BcsvType::integer));

    writer.writeU32(whitehole::smg::jmapHash("label"));
    writer.writeU32(0xFFFFFFFF);
    writer.writeU16(4);
    writer.writeU8(0);
    writer.writeU8(static_cast<std::uint8_t>(whitehole::smg::BcsvType::stringOffset));

    writer.writeU32(42);
    writer.writeU32(0);
    writer.writeString("Comet");
    return std::move(writer).take();
}

void expectTablesEqual(const whitehole::smg::BcsvTable& left, const whitehole::smg::BcsvTable& right,
                       const std::string& context) {
    expect(left.entrySize() == right.entrySize(), context + ": row size changed");
    expect(left.fields().size() == right.fields().size(), context + ": field count changed");
    expect(left.rows().size() == right.rows().size(), context + ": row count changed");
    for (std::size_t index = 0; index < left.fields().size(); ++index) {
        const auto& a = left.fields()[index];
        const auto& b = right.fields()[index];
        expect(a.hash == b.hash && a.mask == b.mask && a.offset == b.offset
                   && a.shift == b.shift && a.type == b.type,
               context + ": field descriptor changed");
    }
    for (std::size_t row = 0; row < left.rows().size(); ++row) {
        expect(left.rows()[row].values.size() == right.rows()[row].values.size(),
               context + ": row width changed");
        for (std::size_t field = 0; field < left.rows()[row].values.size(); ++field) {
            const auto& a = left.rows()[row].values[field];
            const auto& b = right.rows()[row].values[field];
            expect(a.index() == b.index(), context + ": value type changed");
            if (std::holds_alternative<float>(a)) {
                const auto av = std::get<float>(a);
                const auto bv = std::get<float>(b);
                expect(av == bv || (std::isnan(av) && std::isnan(bv)), context + ": float value changed");
            } else {
                expect(a == b, context + ": value changed");
            }
        }
    }
}

void testBcsvEndianness() {
    for (const auto endian : {whitehole::io::Endian::big, whitehole::io::Endian::little}) {
        const whitehole::smg::BcsvTable table(makeTinyBcsv(endian), endian);
        expect(table.fields().size() == 2 && table.rows().size() == 1, "tiny BCSV shape changed");
        expect(std::get<std::int32_t>(table.rows()[0].values[0]) == 42, "BCSV integer parsing failed");
        expect(std::get<std::string>(table.rows()[0].values[1]) == "Comet", "BCSV string parsing failed");
        const whitehole::smg::BcsvTable rewritten(table.serialize(), endian);
        expectTablesEqual(table, rewritten, "tiny BCSV round trip");

        auto invalid = table;
        invalid.rows()[0].values[0] = std::int32_t{70000};
        bool overflowRejected = false;
        try {
            (void)invalid.serialize();
        } catch (const std::runtime_error&) {
            overflowRejected = true;
        }
        expect(overflowRejected, "BCSV silently truncated an integer outside its field mask");

        auto invalidByteMask = table;
        invalidByteMask.fields()[0].type = whitehole::smg::BcsvType::byte;
        invalidByteMask.fields()[0].mask = 0x100;
        invalidByteMask.rows()[0].values[0] = std::int8_t{1};
        bool maskRejected = false;
        try {
            (void)invalidByteMask.serialize();
        } catch (const std::runtime_error&) {
            maskRejected = true;
        }
        expect(maskRejected, "BCSV accepted mask bits outside a field's storage width");
    }
}

void testBcsvMutation() {
    using whitehole::io::Endian;
    using whitehole::smg::BcsvTable;
    using whitehole::smg::BcsvType;

    BcsvTable table(makeTinyBcsv(Endian::big), Endian::big);
    expect(table.hasField("number"), "hasField(number) failed");
    expect(!table.hasField("not_a_field"), "hasField reported a missing field");
    expect(table.rawValue(table.rows()[0], "number") != nullptr, "rawValue(name) failed");

    // Type-aware setters keep the stored variant in step with the field type.
    table.setInt(table.rows()[0], "number", 1234);
    expect(table.getInt(table.rows()[0], "number") == 1234, "setInt/getInt failed");
    table.setBool(table.rows()[0], "number", true);
    expect(table.getBool(table.rows()[0], "number"), "setBool/getBool failed");

    // addRow appends a default-valued row sized to the current schema.
    const auto added = table.addRow();
    expect(added == 1, "addRow should append at index 1");
    expect(table.getInt(table.rows()[added], "number") == 0, "addRow default integer wrong");
    expect(table.getString(table.rows()[added], "label").empty(), "addRow default string wrong");

    // cloneRow copies the source values into a new row right after it.
    table.setInt(table.rows()[0], "number", 7);
    const auto cloned = table.cloneRow(0);
    expect(cloned == 1, "cloneRow should insert after the source row");
    expect(table.getInt(table.rows()[cloned], "number") == 7, "cloneRow did not copy the value");
    expect(table.getString(table.rows()[cloned], "label") == "Comet", "cloneRow did not copy the string");

    // ensureField widens every row and is idempotent.
    const auto flagIndex = table.ensureField("flag", BcsvType::byte);
    expect(table.fields()[flagIndex].type == BcsvType::byte, "ensureField stored the wrong type");
    expect(table.ensureField("flag", BcsvType::integer) == flagIndex, "ensureField is not idempotent");
    for (const auto& row : table.rows()) {
        expect(row.values.size() == table.fields().size(), "ensureField left a ragged row");
    }
    table.setInt(table.rows()[0], "flag", 1);
    expect(table.getInt(table.rows()[0], "flag") == 1, "byte field set/get failed");

    const BcsvTable rewritten(table.serialize(), Endian::big);
    expectTablesEqual(table, rewritten, "mutated BCSV round trip");

    // removeRow reports out-of-range indices instead of corrupting the table.
    const auto before = table.rows().size();
    expect(!table.removeRow(before), "removeRow accepted an out-of-range index");
    expect(table.removeRow(before - 1), "removeRow rejected a valid index");
    expect(table.rows().size() == before - 1, "removeRow did not shrink the table");

    // A table with no schema cannot size a new row.
    BcsvTable empty;
    bool threw = false;
    try {
        (void)empty.addRow();
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "addRow should refuse a table without fields");
}

void testProjectArchives() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    std::size_t archiveCount = 0;
    std::size_t bcsvCount = 0;
    for (const auto& item : std::filesystem::directory_iterator(templates)) {
        if (item.path().extension() != ".arc") {
            continue;
        }
        const auto archive = whitehole::io::RarcArchive::open(item.path());
        expect(archive.wasCompressed(), item.path().string() + " should be Yaz0 compressed");
        expect(!archive.rootName().empty(), item.path().string() + " has no RARC root");
        expect(!archive.entries().empty(), item.path().string() + " has no RARC entries");
        for (const auto& entry : archive.entries()) {
            const auto explicitBcsv = std::filesystem::path(entry.path).extension() == ".bcsv";
            const auto jmapTable = entry.path.find("/jmp/") != std::string::npos;
            if (entry.directory || (!explicitBcsv && !jmapTable)) {
                continue;
            }
            const whitehole::smg::BcsvTable table(archive.read(entry), archive.endian());
            const whitehole::smg::BcsvTable rewritten(table.serialize(), archive.endian());
            expectTablesEqual(table, rewritten, item.path().filename().string() + ":" + entry.path);
            ++bcsvCount;
        }
        const whitehole::io::RarcArchive repacked(archive.serialize(true));
        expect(repacked.wasCompressed(), item.path().string() + " was not recompressed");
        expect(repacked.endian() == archive.endian(), item.path().string() + " endian changed after repacking");
        expect(repacked.entries().size() == archive.entries().size(), item.path().string() + " entry count changed");
        for (std::size_t index = 0; index < archive.entries().size(); ++index) {
            const auto& before = archive.entries()[index];
            const auto& after = repacked.entries()[index];
            expect(before.path == after.path && before.directory == after.directory,
                   item.path().string() + ": archive tree changed after repacking");
            if (!before.directory) {
                expect(archive.read(before) == repacked.read(after),
                       item.path().string() + ": file data changed after repacking");
            }
        }
        ++archiveCount;
    }
    expect(archiveCount == 9, "not every bundled archive template was tested");
    expect(bcsvCount >= 20, "too few bundled BCSV tables were tested");
}

void testArchiveTableEdit() {
    const auto source = std::filesystem::path(WHITEHOLE_SOURCE_DIR)
        / "data" / "templates" / "SMG1OneStarGalaxyScenario.arc";
    auto archive = whitehole::io::RarcArchive::open(source);
    const auto tableEntry = std::find_if(archive.entries().begin(), archive.entries().end(), [](const auto& entry) {
        return !entry.directory && std::filesystem::path(entry.path).filename() == "scenariodata.bcsv";
    });
    expect(tableEntry != archive.entries().end(), "scenario table is missing from the test archive");
    whitehole::smg::BcsvTable table(archive.read(*tableEntry), archive.endian());
    expect(!table.rows().empty() && !table.rows()[0].values.empty(), "scenario table has no editable value");
    table.rows()[0].values[0] = std::int32_t{77};
    archive.replace(tableEntry->path, table.serialize());

    const whitehole::io::RarcArchive saved(archive.serialize(true));
    const auto savedEntry = std::find_if(saved.entries().begin(), saved.entries().end(), [&](const auto& entry) {
        return entry.path == tableEntry->path;
    });
    expect(savedEntry != saved.entries().end(), "edited scenario table disappeared after archive save");
    const whitehole::smg::BcsvTable savedTable(saved.read(*savedEntry), saved.endian());
    expect(std::get<std::int32_t>(savedTable.rows()[0].values[0]) == 77,
           "edited BCSV value did not survive archive recompression");
}

void testUndoStack() {
    using whitehole::edit::IUndo;
    using whitehole::edit::UndoMultiEntry;
    using whitehole::edit::UndoStack;

    // Minimal command that appends text, so stack semantics are observable
    // without touching any game data.
    struct TextCommand final : IUndo {
        TextCommand(std::string* target, std::string text, std::string action)
            : target(target), text(std::move(text)), action(std::move(action)) {}
        void undo() override { target->erase(target->size() - text.size()); }
        void redo() override { *target += text; }
        [[nodiscard]] std::string label() const override { return action; }
        std::string* target;
        std::string text;
        std::string action;
    };

    std::string log;
    UndoStack stack;
    expect(!stack.canUndo() && !stack.canRedo(), "a fresh undo stack should be empty");
    expect(stack.undoLabel().empty(), "an empty undo stack should have no undo label");

    log += "a";
    stack.push(std::make_unique<TextCommand>(&log, "a", "Add a"));
    log += "b";
    stack.push(std::make_unique<TextCommand>(&log, "b", "Add b"));
    expect(log == "ab", "push() must not re-apply an already-applied command");
    expect(stack.size() == 2 && stack.cursor() == 2, "undo stack depth/cursor wrong");
    expect(stack.undoLabel() == "Add b", "undo label should name the newest command");

    expect(stack.undo(), "undo() should report success");
    expect(log == "a", "undo did not revert the newest command");
    expect(stack.canRedo() && stack.redoLabel() == "Add b", "redo label wrong after undo");
    expect(stack.redoCount() == 1, "redo count wrong after undo");

    expect(stack.redo(), "redo() should report success");
    expect(log == "ab", "redo did not re-apply the command");
    expect(stack.redoCount() == 0, "redo count should be zero after redo");

    // Pushing after an undo discards the redo branch.
    expect(stack.undo(), "second undo failed");
    expect(log == "a", "second undo did not revert");
    log += "c";
    stack.push(std::make_unique<TextCommand>(&log, "c", "Add c"));
    expect(!stack.canRedo(), "pushing after an undo should drop the redo branch");
    expect(stack.size() == 2, "the redo branch was not dropped");
    expect(log == "ac", "pushing a new command must not re-apply it");

    // A null entry is ignored rather than crashing.
    stack.push(nullptr);
    expect(stack.size() == 2, "a null undo entry should be ignored");

    stack.clear();
    expect(!stack.canUndo() && stack.size() == 0 && stack.cursor() == 0, "clear did not reset the stack");

    // A multi entry undoes in reverse order and redoes in insertion order.
    std::string multi;
    auto group = std::make_unique<UndoMultiEntry>("Move 2 objects");
    multi += "x";
    group->add(std::make_unique<TextCommand>(&multi, "x", "Add x"));
    multi += "y";
    group->add(std::make_unique<TextCommand>(&multi, "y", "Add y"));
    expect(group->size() == 2 && !group->empty(), "multi entry size wrong");
    expect(group->label() == "Move 2 objects", "multi entry label wrong");
    group->undo();
    expect(multi.empty(), "multi undo should unwind every child");
    group->redo();
    expect(multi == "xy", "multi redo should re-apply every child in order");

    // ---- group capture ---------------------------------------------------
    // Consecutive pushes fold into one undo step while the capture is open.
    log.clear();
    stack.clear();
    stack.beginGroup("Two edits");
    expect(stack.inGroup(), "beginGroup should open a capture");
    log += "x";
    stack.push(std::make_unique<TextCommand>(&log, "x", "Add x"));
    log += "y";
    stack.push(std::make_unique<TextCommand>(&log, "y", "Add y"));
    expect(stack.size() == 0, "grouped pushes must not land on the stack yet");
    expect(!stack.inGroup() == false, "the capture should still be open");
    stack.endGroup();
    expect(!stack.inGroup(), "endGroup should close the capture");
    expect(stack.size() == 1, "a group must be exactly one undo entry");
    expect(stack.undoLabel() == "Two edits", "the group must carry its own label");
    expect(log == "xy", "grouped pushes must not re-apply the commands");
    expect(stack.undo(), "group undo failed");
    expect(log.empty(), "group undo did not unwind every child");
    expect(stack.redo(), "group redo failed");
    expect(log == "xy", "group redo did not re-apply every child");

    // An empty group records nothing.
    const auto settled = stack.size();
    stack.beginGroup("Nothing");
    stack.endGroup();
    expect(stack.size() == settled, "an empty group must not record an undo entry");

    // An unbalanced endGroup is a safe no-op.
    stack.endGroup();
    expect(stack.size() == settled, "an unbalanced endGroup must not touch the stack");

    // A second begin while one is open is ignored: the outer capture owns the
    // pushes, which keeps mis-nested callers from splitting one user action.
    stack.beginGroup("Outer");
    stack.beginGroup("Inner");
    log += "z";
    stack.push(std::make_unique<TextCommand>(&log, "z", "Add z"));
    stack.endGroup();
    expect(stack.undoLabel() == "Outer", "a nested begin must not steal the capture");
    expect(stack.undo(), "outer group undo failed");
    expect(log == "xy", "the outer group did not own its child");
}

void testStageEditCommands() {
    using whitehole::edit::addObject;
    using whitehole::edit::applyRowEdit;
    using whitehole::edit::applyTransform;
    using whitehole::edit::captureRowValues;
    using whitehole::edit::removeObject;
    using whitehole::edit::UndoStack;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = whitehole::smg::StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(!stage.tables().empty() && !stage.objects().empty(), "template stage has nothing to edit");

    UndoStack stack;
    const auto tableIndex = stage.objects()[0].tableIndex;
    const auto rowIndex = stage.objects()[0].rowIndex;
    expect(tableIndex < stage.tables().size(), "first object points at a missing table");

    // ---- transform edit (the drag/rotate/scale case) ---------------------
    const auto before = stage.readObject(tableIndex, rowIndex);
    auto after = before;
    after.position.y = before.position.y + 1.0F;
    expect(applyTransform(stage, stack, {before}, {after}, "Move object"),
           "applyTransform refused a valid edit");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - after.position.y) < 1e-6F,
           "the transform edit did not reach the table");
    expect(stack.undo(), "transform undo failed");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - before.position.y) < 1e-6F,
           "transform undo did not restore the original position");
    expect(stack.redo(), "transform redo failed");
    expect(std::abs(stage.readObject(tableIndex, rowIndex).position.y - after.position.y) < 1e-6F,
           "transform redo did not re-apply the move");

    // ---- property edit through the raw row ------------------------------
    const auto originalRow = captureRowValues(stage, tableIndex, rowIndex);
    auto editedRow = originalRow;
    bool changed = false;
    for (std::size_t field = 0; field < editedRow.size(); ++field) {
        // Only 32-bit integer fields coerce back to the same variant arm, which
        // keeps the round-trip comparison exact.
        if (std::holds_alternative<std::int32_t>(editedRow[field])) {
            editedRow[field] = std::get<std::int32_t>(editedRow[field]) + 1;
            changed = true;
            break;
        }
    }
    expect(changed, "the template row has no integer field to edit");
    expect(applyRowEdit(stage, stack, tableIndex, rowIndex, editedRow, "Edit property"),
           "applyRowEdit refused a valid edit");
    expect(captureRowValues(stage, tableIndex, rowIndex) == editedRow,
           "the property edit did not reach the table");
    expect(stack.undo(), "property undo failed");
    expect(captureRowValues(stage, tableIndex, rowIndex) == originalRow,
           "property undo did not restore the row");
    expect(stack.redo(), "property redo failed");
    expect(captureRowValues(stage, tableIndex, rowIndex) == editedRow,
           "property redo did not re-apply the change");

    // ---- add / remove keep the placement list in step with the tables -----
    const auto objectsBefore = stage.objects().size();
    const auto rowsBefore = stage.tables()[tableIndex].table.rows().size();
    const auto newRow = addObject(stage, stack, tableIndex, editedRow, "Add object");
    expect(newRow == rowsBefore, "an added object should append at the end of its table");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore + 1,
           "addObject did not grow the table");
    expect(stage.objects().size() == objectsBefore + 1,
           "addObject did not grow the placement list");
    expect(stack.undo(), "add undo failed");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore,
           "add undo did not shrink the table");
    expect(stage.objects().size() == objectsBefore, "add undo did not shrink the placement list");
    expect(stack.redo(), "add redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "add redo did not restore the object");

    expect(removeObject(stage, stack, tableIndex, newRow, "Delete object"),
           "removeObject refused a valid edit");
    expect(stage.objects().size() == objectsBefore, "remove did not shrink the placement list");
    expect(stack.undo(), "remove undo failed");
    expect(stage.objects().size() == objectsBefore + 1, "remove undo did not restore the object");

    // Stale indices are refused instead of corrupting the stack.
    const auto depth = stack.size();
    expect(!applyRowEdit(stage, stack, tableIndex, stage.tables()[tableIndex].table.rows().size(),
                         editedRow, "stale"),
           "applyRowEdit accepted an out-of-range row");
    expect(!removeObject(stage, stack, tableIndex, 999999, "stale"),
           "removeObject accepted an out-of-range row");
    expect(stack.size() == depth, "a refused edit must not touch the undo stack");

    // ---- edited data still round-trips through a real save ---------------
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_undo_roundtrip.arc";
    stage.saveTo(saved);
    const auto reopened = whitehole::smg::StageArchive::openMapFile(saved);
    expect(reopened.tables()[tableIndex].table.rows().size()
               == stage.tables()[tableIndex].table.rows().size(),
           "the edited archive did not round-trip its table size");
    expect(reopened.objects().size() == stage.objects().size(),
           "the edited archive did not round-trip its object count");
    std::filesystem::remove(saved);
}

// Object authoring: the native replacement for Java's add-object workflow
// (GalaxyEditorForm.addObject + ObjectSelectForm browsing ObjectDB + the
// ObjIdUtil id scans).
void testObjectAuthoring() {
    using whitehole::edit::createObject;
    using whitehole::edit::deleteObject;
    using whitehole::edit::deleteObjects;
    using whitehole::edit::duplicateObject;
    using whitehole::edit::kindForList;
    using whitehole::edit::listForKind;
    using whitehole::edit::NewObject;
    using whitehole::edit::nextFreeId;
    using whitehole::edit::placementTargets;
    using whitehole::edit::UndoStack;
    using whitehole::smg::StageArchive;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");

    // ---- database placement list <-> native table kind -------------------
    expect(kindForList("MapPartsInfo") == "mappart", "MapPartsInfo must map to the mappart list");
    expect(kindForList("objinfo") == "obj", "list mapping must ignore case");
    expect(kindForList("StartInfo") == "start", "StartInfo must map to the start list");
    expect(listForKind("obj") == "ObjInfo", "obj must map back to ObjInfo");
    expect(listForKind("gravity") == "PlanetObjInfo", "gravity must map back to PlanetObjInfo");
    expect(kindForList("NotAList").empty(), "an unknown list must not map to a table kind");

    // ---- the template's general-object lists -----------------------------
    const auto general = placementTargets(stage, kindForList("ObjInfo"));
    expect(general.size() >= 2, "the template should offer several object layers");
    for (const auto& target : general) {
        expect(target.tableIndex < stage.tables().size(), "a target points at a missing table");
        expect(target.kind == "obj", "a general-object target has the wrong kind");
        expect(target.layer == stage.tables()[target.tableIndex].layer,
               "a target reports the wrong layer");
        expect(target.rowCount == stage.tables()[target.tableIndex].table.rows().size(),
               "a target reports the wrong row count");
    }
    expect(placementTargets(stage).size() == stage.tables().size(),
           "an unfiltered query must list every table");
    expect(placementTargets(stage, "child").empty(), "an absent kind must list nothing");

    UndoStack stack;

    // ---- create ----------------------------------------------------------
    const std::size_t tableIndex = general.front().tableIndex;
    const auto objectsBefore = stage.objects().size();
    const auto rowsBefore = stage.tables()[tableIndex].table.rows().size();

    NewObject request;
    request.name = "AuthorTestObject";
    request.position = {100.0F, 200.0F, 300.0F};
    const auto created = createObject(stage, stack, tableIndex, request);
    expect(created.has_value(), "createObject refused a usable placement table");
    expect(created->rowIndex == rowsBefore, "a new object must append at the end of its table");
    expect(stage.tables()[tableIndex].table.rows().size() == rowsBefore + 1,
           "createObject did not grow the table");
    expect(stage.objects().size() == objectsBefore + 1,
           "createObject did not grow the placement list");
    expect(created->objectIndex < stage.objects().size(), "the created index is out of range");

    {
        const auto& placed = stage.objects()[created->objectIndex];
        expect(placed.name == "AuthorTestObject", "the new object kept the wrong name");
        expect(placed.kind == "obj", "the new object landed in the wrong list");
        expect(placed.layer == general.front().layer, "the new object landed in the wrong layer");
        expect(std::abs(placed.position.x - 100.0F) < 1e-4F &&
                   std::abs(placed.position.y - 200.0F) < 1e-4F &&
                   std::abs(placed.position.z - 300.0F) < 1e-4F,
               "the new object kept the wrong position");
        expect(std::abs(placed.scale.y - 1.0F) < 1e-6F, "a new object must start at scale 1");
        expect(std::abs(placed.rotation.y) < 1e-6F, "a new object must start unrotated");

        // Java's per-type constructors cleared these to -1; a fresh row must too.
        const auto& table = stage.tables()[tableIndex].table;
        const auto& row = table.rows()[created->rowIndex];
        expect(table.getInt(row, "Obj_arg0", 0) == -1, "Obj_arg0 must start at -1 (unset)");
        expect(table.getInt(row, "SW_A", 0) == -1, "SW_A must start at -1 (unset)");
        expect(table.getInt(row, "SW_AWAKE", 0) == -1, "SW_AWAKE must start at -1 (unset)");
        expect(table.getInt(row, "GroupId", 0) == -1, "GroupId must start at -1 (unset)");
        expect(std::abs(table.getFloat(row, "ParamScale", 0.0F) - 1.0F) < 1e-6F,
               "ParamScale must start at 1");

        // The id has to be unique across every row of the same list.
        const auto id = table.getInt(row, "l_id", -1);
        expect(id >= 0, "a new object must carry a usable l_id");
        for (const auto& object : stage.objects()) {
            if (object.tableIndex != tableIndex || object.rowIndex == created->rowIndex) {
                continue;
            }
            expect(table.getInt(table.rows()[object.rowIndex], "l_id", -1) != id,
                   "a new object reused an existing l_id");
        }
    }

    // Undo unwinds the row and the placement list together.
    expect(stack.undo(), "create undo failed");
    expect(stage.objects().size() == objectsBefore, "create undo left the object behind");
    expect(stack.redo(), "create redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "create redo did not restore the object");

    // ---- duplicate -------------------------------------------------------
    const auto sourceIndex = created->objectIndex;
    const auto duplicate = duplicateObject(stage, stack, sourceIndex, {50.0F, 0.0F, 0.0F});
    expect(duplicate.has_value(), "duplicateObject refused a live object");
    expect(duplicate->tableIndex == tableIndex, "a duplicate must stay in its original list");
    expect(duplicate->rowIndex == created->rowIndex + 1,
           "a duplicate must sit right next to its original");
    expect(stage.objects().size() == objectsBefore + 2,
           "duplicateObject did not grow the placement list");
    {
        const auto& copy = stage.objects()[duplicate->objectIndex];
        const auto& original = stage.objects()[sourceIndex];
        expect(copy.name == original.name, "a duplicate must keep the original name");
        expect(std::abs(copy.position.x - (original.position.x + 50.0F)) < 1e-4F,
               "a duplicate must apply the offset");
        expect(std::abs(copy.position.y - original.position.y) < 1e-4F,
               "a duplicate must not move on the untouched axes");
        const auto& table = stage.tables()[tableIndex].table;
        const auto sourceId = table.getInt(table.rows()[original.rowIndex], "l_id", -1);
        const auto copyId = table.getInt(table.rows()[copy.rowIndex], "l_id", -1);
        expect(copyId != sourceId, "a duplicate must take its own l_id");
    }
    expect(stack.undo(), "duplicate undo failed");
    expect(stage.objects().size() == objectsBefore + 1, "duplicate undo left the copy behind");
    expect(stack.redo(), "duplicate redo failed");
    expect(stage.objects().size() == objectsBefore + 2, "duplicate redo did not restore the copy");

    // ---- delete one ------------------------------------------------------
    expect(deleteObject(stage, stack, duplicate->objectIndex),
           "deleteObject refused a live object");
    expect(stage.objects().size() == objectsBefore + 1, "delete did not shrink the placement list");
    expect(stack.undo(), "delete undo failed");
    expect(stage.objects().size() == objectsBefore + 2, "delete undo did not restore the object");
    expect(stack.redo(), "delete redo failed");
    expect(stage.objects().size() == objectsBefore + 1, "delete redo did not remove it again");
    // Undo once more so both objects are present for the multi-delete below.
    expect(stack.undo(), "delete undo (restore) failed");
    expect(stage.objects().size() == objectsBefore + 2, "delete undo did not restore the object");

    // ---- delete several as a single step ---------------------------------
    // The copy's index is re-derived rather than remembered: removing the row
    // above it shifted every index after it, which is exactly why the editor
    // re-syncs its selection after each edit instead of holding indexes across
    // one.
    const auto copyIndex = whitehole::edit::objectIndexAt(stage, tableIndex, created->rowIndex + 1);
    expect(copyIndex.has_value(), "the duplicate vanished from the placement list");

    const auto cursorBefore = stack.cursor();
    // Reverse order plus a repeated index: exactly what a multi-selection with a
    // duplicate entry looks like, and it must still delete the right two rows.
    const auto removedCount =
        deleteObjects(stage, stack, {*copyIndex, sourceIndex, sourceIndex}, "Delete 2 objects");
    expect(removedCount == 2, "deleteObjects removed the wrong number of rows");
    expect(stage.objects().size() == objectsBefore, "deleteObjects left objects behind");
    expect(stack.cursor() == cursorBefore + 1, "a multi-delete must be exactly one undo step");
    expect(stack.redoCount() == 0, "a new edit must discard the redo branch");
    expect(stack.undo(), "multi-delete undo failed");
    expect(stage.objects().size() == objectsBefore + 2,
           "multi-delete undo did not restore both objects");
    expect(stack.redo(), "multi-delete redo failed");
    expect(stage.objects().size() == objectsBefore, "multi-delete redo did not remove both");

    // A stale index is skipped rather than deleting whichever row now sits
    // there, and nothing lands on the undo stack when nothing was removed.
    expect(deleteObjects(stage, stack, {999999}, "Delete nothing") == 0,
           "deleteObjects must ignore indices that no longer exist");
    expect(stack.cursor() == cursorBefore + 1,
           "a multi-delete that removed nothing must not record an undo entry");

    // ---- a start point gets its own MarioNo ------------------------------
    const auto starts = placementTargets(stage, "start");
    if (!starts.empty()) {
        const auto nextMarioNo = nextFreeId(stage, "start", "MarioNo");
        NewObject start;
        start.name = "Mario";
        start.position = {10.0F, 0.0F, 0.0F};
        const auto added = createObject(stage, stack, starts.front().tableIndex, start);
        expect(added.has_value(), "createObject refused the start list");
        const auto& table = stage.tables()[starts.front().tableIndex].table;
        expect(table.getInt(table.rows()[added->rowIndex], "MarioNo", -1) == nextMarioNo,
               "a new start point did not take the next free MarioNo");
        expect(stack.undo(), "start undo failed");
    }

    // ---- an authored archive still round-trips through a real save --------
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_authoring.arc";
    stage.saveTo(saved);
    const auto reopened = StageArchive::openMapFile(saved);
    expect(reopened.tables().size() == stage.tables().size(),
           "the authored archive lost a table on the way to disk");
    expect(reopened.objects().size() == stage.objects().size(),
           "the authored archive round-tripped a different object count");
    std::filesystem::remove(saved);
}

// Group transforms: the mouse-driven editing path that moves, rotates or
// scales a whole selection as one undo entry (gizmo drags, arrow-key nudges).
void testGroupTransforms() {
    using whitehole::edit::rotateObjects;
    using whitehole::edit::scaleObjects;
    using whitehole::edit::translateObjects;
    using whitehole::edit::UndoStack;
    using whitehole::smg::StageArchive;

    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(stage.objects().size() >= 2, "the template has nothing to move as a group");

    UndoStack stack;
    std::vector<std::size_t> selection;
    for (std::size_t index = 0; index < stage.objects().size() && selection.size() < 3; ++index) {
        // Skip start points: translating the player spawn is legal but noisy.
        if (stage.objects()[index].kind != "start") {
            selection.push_back(index);
        }
    }
    expect(selection.size() >= 2, "the template has no moveable pair");

    // ---- translate -------------------------------------------------------
    std::vector<whitehole::smg::PlacementObject> baseline;
    for (const auto index : selection) {
        baseline.push_back(stage.objects()[index]);
    }
    const auto cursorBefore = stack.cursor();
    expect(translateObjects(stage, stack, selection, {10.0F, -5.0F, 2.0F}),
           "translateObjects refused a live selection");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& after = stage.objects()[selection[slot]];
        expect(std::abs(after.position.x - (baseline[slot].position.x + 10.0F)) < 1e-4F &&
                   std::abs(after.position.y - (baseline[slot].position.y - 5.0F)) < 1e-4F &&
                   std::abs(after.position.z - (baseline[slot].position.z + 2.0F)) < 1e-4F,
               "a group translate moved one object wrong");
    }
    expect(stack.cursor() == cursorBefore + 1, "a group translate must be one undo step");
    expect(stack.undo(), "group translate undo failed");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& restored = stage.objects()[selection[slot]];
        expect(std::abs(restored.position.x - baseline[slot].position.x) < 1e-4F,
               "a group translate undo restored the wrong position");
    }
    expect(stack.redo(), "group translate redo failed");

    // ---- a stale index never throws away the rest of the group ------------
    auto stretched = selection;
    stretched.push_back(999999);
    stretched.push_back(selection.front()); // duplicates are moved exactly once
    expect(translateObjects(stage, stack, stretched, {1.0F, 0.0F, 0.0F}, "Jog 2 objects"),
           "a selection with stale entries must still move the rest");
    expect(stack.undoLabel() == "Jog 2 objects", "an explicit label must survive");
    expect(stack.undo(), "stale-selection undo failed");
    // The duplicate entry must not have doubled the move: the row ends up where
    // it started plus exactly one jog.
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& back = stage.objects()[selection[slot]];
        expect(std::abs(back.position.x - (baseline[slot].position.x + 10.0F)) < 1e-4F,
               "a duplicated selection moved one object twice");
    }

    // ---- rotate ----------------------------------------------------------
    expect(rotateObjects(stage, stack, selection, {90.0F, 0.0F, 0.0F}), "rotateObjects refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& turned = stage.objects()[selection[slot]];
        expect(std::abs(turned.rotation.x - (baseline[slot].rotation.x + 90.0F)) < 1e-4F,
               "a group rotate added the wrong degrees");
    }
    expect(stack.undo(), "group rotate undo failed");
    expect(stack.undo(), "undo of the stray redo must also succeed");

    // ---- scale -----------------------------------------------------------
    // A factor of two on every axis must double each scale, and a factor of
    // exactly zero must clamp away from the degenerate point instead.
    expect(scaleObjects(stage, stack, selection, {2.0F, 2.0F, 2.0F}), "scaleObjects refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& grown = stage.objects()[selection[slot]];
        expect(std::abs(grown.scale.x - baseline[slot].scale.x * 2.0F) < 1e-4F,
               "a group scale did not double the stored scale");
    }
    expect(stack.undo(), "group scale undo failed");
    expect(scaleObjects(stage, stack, selection, {0.0F, 0.0F, 0.0F}, "Shrink"), "zero scale refused");
    for (std::size_t slot = 0; slot < selection.size(); ++slot) {
        const auto& shrunk = stage.objects()[selection[slot]];
        expect(std::abs(shrunk.scale.x) > 0.0005F, "a zero factor must clamp before the point");
    }
    expect(stack.undo(), "zero-scale undo failed");

    // ---- nothing valid records nothing -----------------------------------
    expect(!translateObjects(stage, stack, {999998, 999999}, {0.0F, 0.0F, 0.0F}),
           "a fully stale group must fail");
    expect(!rotateObjects(stage, stack, {}, {0.0F, 0.0F, 0.0F}), "an empty group must fail");
    expect(!scaleObjects(stage, stack, {}, {1.0F, 1.0F, 1.0F}), "an empty scale group must fail");
}

// data/objectdb.json is a gitignored first-run download, so a fresh checkout
// does not have it. These tests only need a non-empty database, so fall back to
// a small inline one (same schema as testObjectDatabaseV2) when the file is
// absent; the real community database is used whenever it exists.
void loadObjectDatabaseForTests(whitehole::db::ObjectDatabase& database) {
    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    database.load(root / "data" / "objectdb.json");
    if (!database.empty()) {
        return;
    }
    database.clear();
    database.loadFromJson(R"({
  "Timestamp": 0,
  "Classes": [
    {"InternalName":"SampleObj","Name":"SampleObj","Notes":"A test class","Games":3,"Progress":1,
     "Parameters":{
        "Obj_arg0":{"Name":"Range","Type":"Float","Games":3,"Needed":true,"Description":"How far.","Values":[],"Exclusives":[]},
        "Obj_arg1":{"Name":"Mode","Type":"Integer","Games":3,"Needed":false,"Description":"Pick one.","Values":[{"Value":0,"Notes":"Off"},{"Value":1,"Notes":"On"}],"Exclusives":[]}
     }}
  ],
  "Objects": [
    {"InternalName":"Kinopio","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Toad",
     "Notes":"Friendly.","Category":"npc","ListSMG1":"ObjInfo","ListSMG2":"ObjInfo","File":"Map","Games":3,
     "Progress":1,"IsUnused":false,"IsLeftover":false}
  ]
})");
}

void testObjectModel() {
    using whitehole::db::ObjectDatabase;
    using whitehole::db::PropertyKind;
    using whitehole::edit::UndoStack;
    using whitehole::smg::ObjectModel;
    using whitehole::smg::propertyKindLabel;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);
    expect(!database.empty(), "objectdb.json did not load for the object model test");

    auto stage = whitehole::smg::StageArchive::openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    ObjectModel model(stage, database, 2);
    expect(model.objectCount() > 0, "object model saw no objects");
    expect(model.gameType() == 2, "object model game type wrong");

    // Prefer an object the database knows, so class metadata is exercised.
    std::size_t objectIndex = 0;
    bool classFound = false;
    for (std::size_t index = 0; index < model.objectCount(); ++index) {
        const auto* info = model.objectClass(index);
        if (info != nullptr && !info->properties.empty()) {
            objectIndex = index;
            classFound = true;
            break;
        }
    }

    // Base transform fields are always offered, whatever the database knows.
    const auto fields = model.fields(objectIndex);
    expect(fields.size() >= 10, "object model returned fewer than the base transform fields");
    bool sawName = false;
    bool sawPosX = false;
    for (const auto& field : fields) {
        if (field.identifier == "name") {
            sawName = true;
            expect(field.present, "the name field should be present");
            expect(field.label == "Name", "the name field label is wrong");
            expect(field.kind == PropertyKind::Text, "the name field should be text");
        }
        if (field.identifier == "pos_x") {
            sawPosX = true;
            expect(field.present, "the pos_x field should be present");
            expect(field.kind == PropertyKind::Float, "pos_x should be a float");
        }
    }
    expect(sawName && sawPosX, "the base transform fields are missing from the object model");
    if (classFound) {
        // Class metadata must reach the field list with its human label.
        bool sawMetadata = false;
        for (const auto& field : fields) {
            if (!field.label.empty() && field.label != field.identifier) {
                sawMetadata = true;
                break;
            }
        }
        expect(sawMetadata, "class metadata did not reach the field list");
    }

    // Reads agree with the placement list.
    float posX = 0.0F;
    expect(model.getFloat(objectIndex, "pos_x", posX), "getFloat(pos_x) failed");
    expect(std::abs(posX - stage.objects()[objectIndex].position.x) < 1e-6F,
           "pos_x read disagrees with the placement list");
    std::string name;
    expect(model.getString(objectIndex, "name", name), "getString(name) failed");
    expect(name == stage.objects()[objectIndex].name, "name read disagrees with the placement list");

    // Writes record undo and keep the placement list in step.
    UndoStack stack;
    const auto originalY = stage.objects()[objectIndex].position.y;
    expect(model.setFloat(objectIndex, "pos_y", originalY + 5.0F, stack, "Move object"),
           "setFloat(pos_y) failed");
    expect(stack.size() == 1 && stack.undoLabel() == "Move object",
           "setFloat did not record exactly one named undo entry");
    expect(std::abs(stage.objects()[objectIndex].position.y - (originalY + 5.0F)) < 1e-6F,
           "setFloat did not update the placement list");
    expect(stack.undo(), "undo after setFloat failed");
    expect(std::abs(stage.objects()[objectIndex].position.y - originalY) < 1e-6F,
           "undo did not restore pos_y");
    expect(stack.redo(), "redo after setFloat failed");
    expect(std::abs(stage.objects()[objectIndex].position.y - (originalY + 5.0F)) < 1e-6F,
           "redo did not re-apply pos_y");

    // A no-op set is accepted but must not pollute the undo history.
    const auto depth = stack.size();
    expect(model.setFloat(objectIndex, "pos_y", originalY + 5.0F, stack, "no-op"),
           "a no-op setFloat should still report success");
    expect(stack.size() == depth, "a no-op edit must not add an undo entry");

    // Absent fields and out-of-range objects are refused.
    float unused = 0.0F;
    expect(!model.getFloat(objectIndex, "definitely_not_a_field", unused),
           "getFloat accepted a missing field");
    expect(!model.setFloat(objectIndex, "definitely_not_a_field", 1.0F, stack, "bad"),
           "setFloat accepted a missing field");
    expect(!model.setFloat(999999, "pos_x", 1.0F, stack, "bad"),
           "setFloat accepted an out-of-range object");

    // A typed accessor rejects the wrong storage type.
    std::string text;
    expect(!model.getString(objectIndex, "pos_x", text), "getString should reject a float field");

    expect(propertyKindLabel(PropertyKind::Float) == "float", "propertyKindLabel(float) is wrong");
    expect(propertyKindLabel(PropertyKind::SwitchId) == "switch", "propertyKindLabel(switch) is wrong");
}

void testValidation() {
    using whitehole::db::ObjectDatabase;
    using whitehole::edit::Severity;
    using whitehole::edit::toString;
    using whitehole::edit::validateStage;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);
    expect(!database.empty(), "objectdb.json did not load for the validation test");

    auto stage = whitehole::smg::StageArchive::openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    expect(!stage.objects().empty(), "template stage has no objects to validate");

    // An empty database means "nothing to validate against", not a wall of errors.
    ObjectDatabase blank;
    expect(validateStage(stage, blank, 2).empty(),
           "validation should stay silent without a database");

    // Force two known problems on the first object.
    auto& objects = stage.objects();
    objects[0].name = "WhiteholeDefinitelyNotAnObject";
    objects[0].scale = {0.0F, 0.0F, 0.0F};
    stage.applyEdits();

    const auto report = validateStage(stage, database, 2);
    expect(!report.findings.empty(), "validation found nothing in a stage with a bogus object");
    expect(report.findings.size() == report.count(Severity::Info) + report.count(Severity::Warning)
               + report.count(Severity::Error),
           "finding severities do not add up to the finding count");
    expect(!report.clean(), "a report with warnings should not be clean");

    bool unknown = false;
    bool zeroScale = false;
    for (const auto& finding : report.findings) {
        if (finding.code == "unknown-object" && finding.objectIndex == 0 && !finding.hint.empty()) {
            unknown = true;
        }
        if (finding.code == "zero-scale" && finding.objectIndex == 0) {
            zeroScale = true;
        }
    }
    expect(unknown, "the bogus object was not reported as unknown");
    expect(zeroScale, "zero scale was not reported");

    expect(toString(Severity::Warning) == "warning", "the warning severity name is wrong");
    expect(toString(Severity::Error) == "error", "the error severity name is wrong");
}

void testDocument() {
    using whitehole::db::ObjectDatabase;
    using whitehole::edit::Document;
    using whitehole::edit::Severity;

    const auto root = std::filesystem::path(WHITEHOLE_SOURCE_DIR);
    ObjectDatabase database;
    loadObjectDatabaseForTests(database);

    Document document;
    document.setDatabase(&database);
    expect(!document.hasStage(), "a fresh document should have no stage");
    expect(!document.dirty(), "a fresh document should be clean");

    int changes = 0;
    document.setOnChange([&changes] { ++changes; });

    document.openMapFile(root / "data" / "templates" / "SMG2BigGalaxyMap.arc");
    expect(document.hasStage(), "openMapFile did not open a stage");
    expect(!document.zoneName().empty(), "openMapFile did not set the zone name");
    expect(!document.dirty(), "opening a file must not mark the document dirty");
    expect(changes > 0, "opening a file should notify observers");
    expect(document.stage() != nullptr && !document.stage()->objects().empty(),
           "the opened stage has no objects");

    // Selection is multi-select aware, and selecting is not an edit.
    document.select(0);
    expect(document.selection().size() == 1 && document.isSelected(0), "single selection failed");
    document.select(1, true);
    expect(document.selection().size() == 2 && document.isSelected(1), "additive selection failed");
    document.select(1, true);
    expect(document.selection().size() == 2, "additive selection duplicated an index");
    document.clearSelection();
    expect(document.selection().empty(), "clearSelection failed");
    expect(!document.dirty(), "selection changes must not mark the document dirty");

    // An edit is undoable and flips the dirty flag; undoing back is clean again.
    const auto originalY = document.stage()->objects()[0].position.y;
    auto model = document.objectModel();
    expect(model.setFloat(0, "pos_y", originalY + 3.0F, document.undoStack(), "Move object"),
           "editing through the document failed");
    expect(document.dirty(), "an edit should mark the document dirty");
    expect(document.canUndo() && document.undoLabel() == "Move object", "undo state is wrong");
    expect(document.undo(), "document.undo() failed");
    expect(!document.dirty(), "undoing back to the save point should report clean");
    expect(!document.undo(), "undo should stop at the start of history");
    expect(document.redo(), "document.redo() failed");
    expect(document.dirty(), "redo should mark the document dirty again");

    // Saving records a new save point and round-trips the change.
    const auto saved = std::filesystem::temp_directory_path() / "whitehole_document_roundtrip.arc";
    document.saveAs(saved);
    expect(!document.dirty(), "save should clear the dirty flag");
    const auto reopened = whitehole::smg::StageArchive::openMapFile(saved);
    expect(std::abs(reopened.objects()[0].position.y - (originalY + 3.0F)) < 1e-6F,
           "the saved document did not round-trip the edit");
    std::filesystem::remove(saved);

    // Validation is reachable straight from the document.
    const auto report = document.validate();
    expect(report.findings.size() == report.count(Severity::Info) + report.count(Severity::Warning)
               + report.count(Severity::Error),
           "document validation severities do not add up");

    document.close();
    expect(!document.hasStage() && !document.dirty(), "close did not reset the document");
}

void testNameTables() {
    whitehole::db::NameTable galaxies;
    galaxies.loadJson(std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "galaxies.json");
    expect(galaxies.displayName("EggStarGalaxy").find("Good Egg") != std::string::npos,
           "galaxy display names did not load");
}

void testStageAndGameModels() {
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = whitehole::smg::StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(!stage.objects().empty(), "template map loaded no placement objects");
    expect(stage.objects().size() >= 10, "template map loaded fewer objects than expected");
    auto& object = stage.objects().front();
    const auto original = object.position.x;
    object.position.x = original + 12.5F;
    TemporaryDirectory output;
    const auto savedPath = output.path / "edited-map.arc";
    stage.saveTo(savedPath);
    const auto reloaded = whitehole::smg::StageArchive::openMapFile(savedPath);
    expect(!reloaded.objects().empty(), "saved map reloaded no objects");
    expect(std::abs(reloaded.objects().front().position.x - (original + 12.5F)) < 0.01F,
           "edited object position did not survive save");

    TemporaryDirectory workspace;
    std::filesystem::create_directories(workspace.path / "SystemData");
    std::filesystem::create_directories(workspace.path / "StageData" / "TestGalaxy");
    whitehole::io::writeFile(workspace.path / "SystemData" / "ObjNameTable.arc", {0x52, 0x41, 0x52, 0x43});
    std::filesystem::copy_file(templates / "SMG2BigGalaxyScenario.arc",
                               workspace.path / "StageData" / "TestGalaxy" / "TestGalaxyScenario.arc");
    std::filesystem::copy_file(templates / "SMG2BigGalaxyMap.arc",
                               workspace.path / "StageData" / "TestGalaxy" / "TestGalaxyMap.arc");
    whitehole::smg::GameArchive game(workspace.path);
    expect(game.gameType() == 2, "synthetic workspace was not detected as SMG2");
    expect(game.galaxyExists("TestGalaxy"), "synthetic galaxy was not listed");
    const auto galaxy = game.openGalaxy("TestGalaxy");
    expect(!galaxy.zones().empty(), "synthetic galaxy has no zones");
}

void testBtiDecoding() {
    using whitehole::io::Endian;
    using whitehole::smg::decodeBtiImage;
    using whitehole::smg::parseBti;

    // RGB565 (format 4): 4x4 of full-white word 0xFFFF -> opaque white.
    {
        std::vector<std::uint8_t> data(32, 0xFF);
        auto img = decodeBtiImage(data, 0, 4, 4, 4, 0, Endian::big);
        expect(img.width == 4 && img.height == 4, "RGB565 dimensions were not 4x4");
        expect(img.rgba.size() == 4u * 4u * 4u, "RGB565 rgba byte count was wrong");
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB565 0xFFFF should decode to white opaque");
    }

    // RGB5A3 opaque (format 5, bit 15 set): 0xFFFF -> white opaque. One 4x4 block = 32 bytes.
    {
        std::vector<std::uint8_t> data(32, 0xFF);
        auto img = decodeBtiImage(data, 0, 5, 2, 2, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB5A3 opaque 0xFFFF should be white opaque");
    }

    // RGB5A3 ARGB3444 (format 5, bit 15 clear): exercises the expand4 fix.
    {
        std::vector<std::uint8_t> maxWord(32, 0);
        maxWord[0] = 0x7F; maxWord[1] = 0xFF; // a=7, r=15, g=15, b=15
        auto img = decodeBtiImage(maxWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "RGB5A3 ARGB3444 max should be opaque white");
        std::vector<std::uint8_t> blueWord(32, 0);
        blueWord[0] = 0x00; blueWord[1] = 0x0F; // blue nibble 15, alpha nibble 0
        auto img2 = decodeBtiImage(blueWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img2.rgba[2] == 255, "RGB5A3 blue nibble 0xF must expand to 255 (expand4 fix)");
        expect(img2.rgba[3] == 0, "RGB5A3 alpha nibble 0 must expand to 0");
        std::vector<std::uint8_t> midWord(32, 0);
        midWord[0] = 0x00; midWord[1] = 0x05; // blue nibble 5
        auto img3 = decodeBtiImage(midWord, 0, 5, 1, 1, 0, Endian::big);
        expect(img3.rgba[2] == 85, "RGB5A3 nibble 0x5 must expand to 85 (expand4)");
    }


    // IA4 (format 2): 2x2 inside one 8x4 block (32 bytes consumed).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x55; // intensity 5, alpha 5 -> expand4(5) = 85
        data[1] = 0xFF; // intensity 15, alpha 15 -> 255
        auto img = decodeBtiImage(data, 0, 2, 2, 2, 0, Endian::big);
        const std::size_t p00 = (0u * img.width + 0u) * 4u;
        const std::size_t p10 = (0u * img.width + 1u) * 4u;
        expect(img.rgba[p00] == 85 && img.rgba[p00 + 3] == 85, "IA4 nibble 5 should expand to 85");
        expect(img.rgba[p10] == 255, "IA4 nibble 15 should expand to 255");
    }

    // I8 (format 1): 2x2 inside one 8x4 block (32 bytes consumed).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x80;
        data[1] = 0x10;
        data[8] = 0xFF;
        data[9] = 0x00;
        auto img = decodeBtiImage(data, 0, 1, 2, 2, 0, Endian::big);
        expect(img.rgba[0] == 0x80 && img.rgba[3] == 255, "I8 pixel (0,0) was wrong");
        expect(img.rgba[8] == 0xFF, "I8 pixel (0,1) was wrong");
        expect(img.rgba[12] == 0x00 && img.rgba[15] == 255, "I8 pixel (1,1) was wrong");
    }

    // C8 (format 9) palettized: rgb565 palette, entry 0 white, entry 1 black.
    {
        std::vector<std::uint8_t> palette = {0xFF, 0xFF, 0x00, 0x00};
        std::vector<std::uint8_t> data(32, 0);
        data[1] = 1;
        data[9] = 1;
        auto img = decodeBtiImage(data, 0, 9, 2, 2, 0, Endian::big, palette, 1);
        expect(img.rgba[0] == 255 && img.rgba[3] == 255, "C8 palette entry 0 should map white");
        expect(img.rgba[4] == 0 && img.rgba[7] == 255, "C8 palette entry 1 should map black");
        expect(img.rgba[8] == 255, "C8 palette entry 0 for pixel (0,1) wrong");
        expect(img.rgba[12] == 0 && img.rgba[15] == 255, "C8 palette entry 1 for pixel (1,1) wrong");
    }

    // CMPR (format 14): one 8x8 macro block = 32 bytes; colorA > colorB -> color3 = (2*c0+c1)/3.
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0xFF; data[1] = 0xFF; // colorA = 0xFFFF (white)
        data[4] = 0x80;                 // pixel(0,0) index 2 -> color3 = 170
        auto img = decodeBtiImage(data, 0, 14, 4, 4, 0, Endian::big);
        expect(img.rgba[0] == 170 && img.rgba[1] == 170 && img.rgba[2] == 170 && img.rgba[3] == 255,
               "CMPR color3 interpolation was wrong");
    }

    // CMPR: colorA <= colorB -> fourth color is transparent (alpha 0).
    {
        std::vector<std::uint8_t> data(32, 0);
        data[2] = 0xFF; data[3] = 0xFF; // colorB = 0xFFFF (white), colorA = 0
        data[4] = 0xC0;                 // pixel(0,0) index 3 -> color4 = transparent white
        auto img = decodeBtiImage(data, 0, 14, 4, 4, 0, Endian::big);
        expect(img.rgba[0] == 255 && img.rgba[1] == 255 && img.rgba[2] == 255 && img.rgba[3] == 0,
               "CMPR transparent color4 must have alpha 0");
    }

    // parseBti: standalone .bti entry at entryOffset 0 (I8, 4x4).
    {
        std::vector<std::uint8_t> blob(64, 0);
        blob[0] = 1;
        blob[2] = 0; blob[3] = 4;
        blob[4] = 0; blob[5] = 4;
        blob[24] = 0;
        blob[28] = 0; blob[29] = 0; blob[30] = 0; blob[31] = 32;
        for (int i = 0; i < 16; ++i) {
            blob[32 + i] = static_cast<std::uint8_t>(i * 8 + i);
        }
        const auto bti = parseBti(blob, 0, Endian::big);
        expect(bti.width == 4 && bti.height == 4, "parseBti width/height wrong");
        expect(bti.mipmaps.size() == 1, "parseBti should decode one mip level");
        expect(bti.mipmaps[0].rgba.size() == 4u * 4u * 4u, "parseBti mip byte count wrong");
        expect(bti.mipmaps[0].rgba[0] == 0 && bti.mipmaps[0].rgba[3] == 255, "parseBti I8 pixel (0,0) wrong");
    }

    // parseBti: embedded entry at a NON-zero entryOffset (absolute offset fix).
    {
        constexpr std::size_t prefix = 16;
        std::vector<std::uint8_t> blob(prefix + 32 + 32, 0);
        blob[prefix + 0] = 1;
        blob[prefix + 2] = 0; blob[prefix + 3] = 2;
        blob[prefix + 4] = 0; blob[prefix + 5] = 2;
        blob[prefix + 24] = 0;
        blob[prefix + 28] = 0; blob[prefix + 29] = 0; blob[prefix + 30] = 0; blob[prefix + 31] = 32;
        blob[prefix + 32 + 0] = 0x80;
        blob[prefix + 32 + 1] = 0x10;
        blob[prefix + 32 + 8] = 0xFF;
        blob[prefix + 32 + 9] = 0x00;
        const auto bti = parseBti(blob, prefix, Endian::big);
        expect(bti.width == 2 && bti.height == 2, "embedded parseBti dimensions wrong");
        expect(bti.mipmaps.size() == 1, "embedded parseBti mip count wrong");
        expect(bti.mipmaps[0].rgba[0] == 0x80 && bti.mipmaps[0].rgba[3] == 255, "embedded parseBti (0,0) wrong");
        expect(bti.mipmaps[0].rgba[8] == 0xFF, "embedded parseBti (0,1) wrong");
        expect(bti.mipmaps[0].rgba[12] == 0x00 && bti.mipmaps[0].rgba[15] == 255, "embedded parseBti (1,1) wrong");
    }
}


void testJsonRoundTrip() {
    using whitehole::util::JsonArray;
    using whitehole::util::JsonObject;
    using whitehole::util::JsonValue;
    JsonObject root;
    root["name"] = JsonValue("Kinopio");
    root["count"] = JsonValue(3.0);
    root["ok"] = JsonValue(true);
    JsonArray items;
    items.emplace_back("a");
    items.emplace_back(1.0);
    root["items"] = JsonValue(std::move(items));
    const std::string text = whitehole::util::serializeJson(JsonValue(root));
    const JsonValue parsed = whitehole::util::parseJson(text);
    expect(parsed.strAt("name") == "Kinopio", "JSON string round trip failed");
    expect(parsed.at("count").asNumber() == 3.0, "JSON number round trip failed");
    expect(parsed.at("ok").asBool() == true, "JSON bool round trip failed");
    expect(parsed.at("items").asArray().size() == 2, "JSON array round trip failed");
    bool rejected = false;
    try {
        (void)whitehole::util::parseJson("{bad}");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "malformed JSON was not rejected");
}

void testSettingsRoundTrip() {
    whitehole::app::Settings settings;
    TemporaryDirectory temp;
    settings.setConfigPath(temp.path / "settings.json");
    settings.lastGameDir = "C:/games/smg2";
    settings.darkMode = false;
    settings.showPaths = false;
    settings.pushRecentMap("C:/games/map.arc");
    settings.pushRecentMap("C:/games/other.arc");
    settings.pushRecentMap("C:/games/map.arc");
    expect(settings.recentMaps.size() == 2, "recent maps should dedupe");
    expect(settings.recentMaps.front() == "C:/games/map.arc", "recent maps order wrong");
    settings.save();
    whitehole::app::Settings loaded;
    loaded.setConfigPath(temp.path / "settings.json");
    loaded.load();
    expect(loaded.lastGameDir == "C:/games/smg2", "settings lastGameDir mismatch");
    expect(loaded.darkMode == false, "settings darkMode mismatch");
    expect(loaded.showPaths == false, "settings showPaths mismatch");
    expect(loaded.recentMaps.size() == 2, "settings recentMaps mismatch");
}

void testObjectDatabase() {
    TemporaryDirectory temp;
    const auto path = temp.path / "objectdb.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"Objects\":["
               "{\"InternalName\":\"Kinopio\",\"SimpleName\":\"Toad\",\"Category\":\"NPC\"},"
               "{\"InternalName\":\"Kuribo\",\"Category\":\"Enemy\"}]}";
    }
    whitehole::db::ObjectDatabase db;
    db.load(path);
    expect(db.size() == 2, "objectdb size wrong");
    expect(db.contains("Kinopio"), "objectdb missing Kinopio");
    expect(db.displayName("Kinopio") == "Toad", "objectdb display name wrong");
    expect(db.displayName("Missing") == "\"Missing\"", "objectdb fallback wrong");
}

void testDataHolderRoundTrip() {
    TemporaryDirectory temp;
    const auto base = temp.path / "base";
    std::filesystem::create_directories(base);
    {
        std::ofstream out(base / "test.json");
        out << "{\"Items\": [\"a\",\"b\"]}";
    }
    whitehole::db::DataHolderBase holder("test.json", "/hints.json", true);
    holder.setBaseGameRoot(base);
    holder.initBaseGame();
            expect(holder.dataPresent(), "base game data should be present");
    expect(holder.root().at("Items").asArray().size() == 2, "base root should parse");
}

void testDbHelpersRoundTrip() {
        TemporaryDirectory temp;
    const auto base = temp.path / "base";
    std::filesystem::create_directories(base / "data");

    // Hints: top-level "Hints" array with Game field for filtering.
    {
        std::ofstream out(base / "data" / "hints.json");
        out << "{\"Hints\":[{\"Game\":0,\"Hint\":\"hello\",\"Name\":\"Test\"},{\"Game\":1,\"Hint\":\"smg1only\"}]}";
    }
    whitehole::db::Hints hints;
    hints.setBaseGameRoot(base);
    hints.initBaseGame();
    hints.load(2);
    expect(hints.hints().size() == 1, "hints should load one entry for SMG2");
    expect(hints.hints()[0].hint == "hello", "hints hint wrong");
    hints.load(1);
    expect(hints.hints().size() == 2, "hints should load two entries for SMG1");

    // AreaManagerLimits: aliases + limits per game.
    {
        std::ofstream out(base / "data" / "areamanagerlimits.json");
        out << "{\"AreaManagerAliases\":{\"SMG1\":{\"Cube\":\"Area\"}},\"AreaManagers\":{\"SMG1\":{\"Area\":\"5\"}}}";
    }
    whitehole::db::AreaManagerLimits limits;
    limits.setBaseGameRoot(base);
    limits.initBaseGame();
    limits.load(1);
    expect(limits.resolveAlias("Cube") == "Area", "alias resolve wrong");
    expect(limits.resolveAlias("Missing") == "Missing", "alias passthrough wrong");
    expect(limits.limitFor("Cube") == "5", "limit via alias wrong");
    expect(limits.limitFor("Missing") == "", "limit missing empty");

    // Shortcuts: flat root.
    {
        std::ofstream out(base / "data" / "shortcuts.json");
        out << "{\"save\":\"Ctrl+S\"}";
    }
    whitehole::db::Shortcuts shortcuts;
    shortcuts.setBaseGameRoot(base);
    shortcuts.initBaseGame();
    shortcuts.load();
    expect(shortcuts.get("save") == "Ctrl+S", "shortcut get wrong");
    expect(shortcuts.get("missing") == "", "shortcut missing empty");

    // ModelSubstitutions: flat lowercase-keyed map.
    {
        std::ofstream out(base / "data" / "modelsubstitutions.json");
        out << "{\"dumptr\":\"Kinopio\"}";
    }
    whitehole::db::ModelSubstitutions subs;
    subs.setBaseGameRoot(base);
    subs.initBaseGame();
    subs.load();
    expect(subs.substitute("Dumptr") == "Kinopio", "case-insensitive substitute");
    expect(subs.substitute("missing") == "", "no substitute empty");

    // SpecialRenderers: array with ObjectName/ClassName + RendererType.
    {
        std::ofstream out(base / "data" / "specialrenderers.json");
        out << "{\"SpecialRenderers\":[{\"ObjectName\":\"GoalA\",\"RendererType\":\"Special\"}]}";
    }
    whitehole::db::SpecialRenderers special;
    special.setBaseGameRoot(base);
    special.initBaseGame();
    special.load();
    expect(special.lookup("GoalA") == "Special", "special renderer lookup");
    expect(special.lookup("Unknown") == "", "special renderer unknown empty");
}

void testObjectDatabaseV2() {
    TemporaryDirectory temp;
    const auto path = temp.path / "objectdb.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({
  "Timestamp": 1234567890,
  "Categories": [{"Key":"enemy","Description":"Enemies"},{"Key":"stagepart","Description":"Stage Parts"}],
  "Classes": [
    {"InternalName":"SampleObj","Name":"SampleObj","Notes":"A test class","Games":3,"Progress":1,
     "Parameters":{
        "Obj_arg0":{"Name":"Range","Type":"Float","Games":3,"Needed":true,"Description":"How far.","Values":[],"Exclusives":[]},
        "Obj_arg1":{"Name":"Mode","Type":"Integer","Games":3,"Needed":false,"Description":"Pick one.","Values":[{"Value":0,"Notes":"Off"},{"Value":1,"Notes":"On"}],"Exclusives":[]},
        "Obj_arg2":{"Name":"Only SMG2","Type":"Integer","Games":2,"Needed":false,"Description":"","Values":[],"Exclusives":[]},
        "Obj_arg3":{"Name":"Special","Type":"Boolean","Games":3,"Needed":false,"Description":"","Values":[],"Exclusives":["Kinopio"]},
        "SW_A":{"Games":3,"Needed":false,"Description":"Switch A.","Values":[],"Exclusives":[]}
     }}
  ],
  "Objects": [
    {"InternalName":"Kinopio","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Toad",
     "Notes":"Friendly.","Category":"npc","ListSMG1":"ObjInfo","ListSMG2":"ObjInfo","File":"Map","Games":3,
     "Progress":1,"IsUnused":false,"IsLeftover":false},
    {"InternalName":"OldThing","ClassNameSMG1":"SampleObj","ClassNameSMG2":"SampleObj","Name":"Leftover",
     "Category":"stagepart","Games":1,"IsUnused":true,"IsLeftover":true}
  ]
})";
    }
    whitehole::db::ObjectDatabase db;
    db.load(path);

    expect(db.size() == 2, "objectdb v2 object count wrong");
    expect(db.classCount() == 1, "objectdb v2 class count wrong");
    expect(db.categoryCount() == 2, "objectdb v2 category count wrong");
    expect(db.timestamp() == 1234567890U, "objectdb v2 timestamp wrong");

    const auto* kinopio = db.find("Kinopio");
    expect(kinopio != nullptr, "objectdb v2 missing Kinopio");
    expect(kinopio->name == "Toad", "objectdb v2 display name wrong");
    expect(kinopio->description == "Friendly.", "objectdb v2 notes wrong");
    expect(kinopio->className(1) == "SampleObj", "objectdb v2 smg1 class wrong");
    expect(kinopio->className(2) == "SampleObj", "objectdb v2 smg2 class wrong");
    expect(kinopio->list(2) == "ObjInfo", "objectdb v2 list wrong");
    expect(!kinopio->unused, "objectdb v2 unused flag wrong");

    expect(db.findClass("SampleObj") != nullptr, "objectdb v2 class lookup failed");
    expect(db.classForObject("Kinopio", 2) != nullptr, "objectdb v2 class-for-object failed");

    // Labels, descriptions and kinds come from the class metadata.
    expect(db.propertyLabel("Kinopio", "Obj_arg0", 2) == "Range", "objectdb v2 label wrong");
    expect(db.propertyDescription("Kinopio", "Obj_arg0", 2) == "How far.", "objectdb v2 description wrong");
    const auto* range = db.propertyForObject("Kinopio", "Obj_arg0", 2);
    expect(range != nullptr, "objectdb v2 float property missing");
    expect(range->kind == whitehole::db::PropertyKind::Float, "objectdb v2 float kind wrong");
    expect(range->needed, "objectdb v2 needed flag wrong");

    const auto* mode = db.propertyForObject("Kinopio", "Obj_arg1", 2);
    expect(mode != nullptr, "objectdb v2 list property missing");
    expect(mode->kind == whitehole::db::PropertyKind::IntList, "objectdb v2 list kind wrong");
    expect(mode->values.size() == 2, "objectdb v2 values count wrong");
    expect(mode->values[1] == "1: On", "objectdb v2 value formatting wrong");

    // Per-game filtering: Obj_arg2 only exists in SMG2.
    expect(db.propertyUsed("Kinopio", "Obj_arg2", 2), "objectdb v2 smg2-only property hidden");
    expect(!db.propertyUsed("Kinopio", "Obj_arg2", 1), "objectdb v2 smg2-only property leaked to smg1");

    // Exclusives: Obj_arg3 is listed for Kinopio only.
    expect(db.propertyUsed("Kinopio", "Obj_arg3", 2), "objectdb v2 exclusive property hidden");
    expect(!db.propertyUsed("OldThing", "Obj_arg3", 2), "objectdb v2 exclusive property leaked");

    // A parameter with no declared "Type" still becomes an integer cell, and an
    // unknown parameter falls back to its raw identifier as the label.
    const auto* switchA = db.propertyForObject("Kinopio", "SW_A", 2);
    expect(switchA != nullptr, "objectdb v2 untyped property missing");
    expect(switchA->kind == whitehole::db::PropertyKind::Integer, "objectdb v2 untyped kind wrong");
    expect(db.propertyLabel("Kinopio", "SomethingElse", 2) == "SomethingElse",
           "objectdb v2 unknown property label wrong");

    // Alias table (Java getPropertyInfoForObject).
    expect(whitehole::db::ObjectDatabase::aliasField("CommonPath_ID") == "Rail", "alias Rail wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("CameraSetId") == "Camera", "alias Camera wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("GroupId") == "Group", "alias Group wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("MessageId") == "Message", "alias Message wrong");
    expect(whitehole::db::ObjectDatabase::aliasField("Plain") == "Plain", "alias passthrough wrong");

    // Game availability (Java ObjectSelectForm filter).
    expect(db.objectAvailable("Kinopio", 1) && db.objectAvailable("Kinopio", 2),
           "objectdb v2 availability wrong");
    expect(db.objectAvailable("OldThing", 1), "objectdb v2 smg1-only availability wrong");
    expect(!db.objectAvailable("OldThing", 2), "objectdb v2 smg1-only leaked to smg2");

    // Search matches display name, internal name and class name.
    expect(db.search("toad", 2).size() == 1, "objectdb v2 search by display name");
    expect(db.search("kinop", 2).size() == 1, "objectdb v2 search by internal name");
    expect(db.search("sampleobj", 1).size() == 2, "objectdb v2 search by class name");
    expect(db.search("nothinghere", 2).empty(), "objectdb v2 search false positive");

    // Categories keep declaration order.
    expect(db.categories()[0].key == "enemy", "objectdb v2 category order wrong");
    expect(db.categories()[1].description == "Stage Parts", "objectdb v2 category description wrong");
}

void testObjectDatabaseCache() {
    TemporaryDirectory temp;
    const auto jsonPath = temp.path / "objectdb.json";
    const auto cachePath = temp.path / "cache" / "objectdb.cache";

    const auto writeJson = [&](std::string_view name) {
        std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
        out << "{\"Timestamp\":7,\"Classes\":[{\"InternalName\":\"C\",\"Parameters\":{}}],"
               "\"Objects\":[{\"InternalName\":\"Obj\",\"Name\":\""
            << name << "\",\"ClassNameSMG1\":\"C\",\"ClassNameSMG2\":\"C\",\"Games\":3}]}";
    };

    writeJson("First");
    whitehole::db::ObjectDatabase db;
    db.load(jsonPath, cachePath);
    expect(!db.cacheLoaded(), "objectdb cache should not be used before it exists");
    expect(db.displayName("Obj") == "First", "objectdb cache source value wrong");
    expect(std::filesystem::exists(cachePath), "objectdb cache was not written");

    whitehole::db::ObjectDatabase cached;
    cached.load(jsonPath, cachePath);
    expect(cached.cacheLoaded(), "objectdb cache was not used on the second load");
    expect(cached.size() == 1, "objectdb cache object count wrong");
    expect(cached.classCount() == 1, "objectdb cache class count wrong");
    expect(cached.displayName("Obj") == "First", "objectdb cache display name wrong");
    expect(cached.timestamp() == 7U, "objectdb cache timestamp wrong");

    // Rewriting the JSON must invalidate the compiled cache. The source
    // timestamp is pushed into the future so the test does not depend on clock
    // granularity.
    writeJson("Second");
    std::error_code error;
    const auto bumped = std::filesystem::last_write_time(jsonPath, error) + std::chrono::seconds(10);
    std::filesystem::last_write_time(jsonPath, bumped, error);

    whitehole::db::ObjectDatabase reparsed;
    reparsed.load(jsonPath, cachePath);
    expect(!reparsed.cacheLoaded(), "objectdb cache was not invalidated");
    expect(reparsed.displayName("Obj") == "Second", "objectdb cache invalidation value wrong");
}

void testRealObjectDatabase() {
#ifdef WHITEHOLE_SOURCE_DIR
    const auto path = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "objectdb.json";
    if (!std::filesystem::exists(path)) return; // optional bulky data

    whitehole::db::ObjectDatabase db;
    db.load(path);
    expect(db.size() > 2000, "real objectdb object count unexpectedly small");
    expect(db.classCount() > 800, "real objectdb class count unexpectedly small");
    expect(db.categoryCount() >= 10, "real objectdb categories unexpectedly few");
    expect(db.timestamp() > 0, "real objectdb timestamp missing");

    // Whenever the database declares a class name for an object it must resolve,
    // otherwise the property grid would silently come up empty.
    std::size_t declared = 0;
    std::size_t resolved = 0;
    for (const auto& name : db.names()) {
        const auto* info = db.find(name);
        if (info == nullptr || info->classNameSmg2.empty()) continue;
        ++declared;
        if (db.findClass(info->classNameSmg2) != nullptr) ++resolved;
    }
    expect(declared > 2000, "real objectdb declares too few SMG2 class names");
    expect(resolved == declared, "real objectdb has unresolvable SMG2 class names");

    const auto* rail = db.findClass("RailMoveObj");
    expect(rail != nullptr, "real objectdb missing the RailMoveObj class");
    expect(!rail->properties.empty(), "real objectdb RailMoveObj has no parameters");
#endif
}

// ---------------------------------------------------------------------------
// BMD/BDL reading and model mesh building.
//
// The fixtures below are assembled by absolute offset because that is exactly
// how J3D stores data: every array is placed by an explicit offset table, and
// the reader has to trust those tables. Building them by hand keeps the tests
// independent of any real game file.
// ---------------------------------------------------------------------------

void putU16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value) {
    if (data.size() < offset + 2) {
        data.resize(offset + 2, 0);
    }
    data[offset] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

void putU32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    if (data.size() < offset + 4) {
        data.resize(offset + 4, 0);
    }
    data[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

void putF32(std::vector<std::uint8_t>& data, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(data, offset, bits);
}

void putText(std::vector<std::uint8_t>& data, std::size_t offset, std::string_view text) {
    if (data.size() < offset + text.size()) {
        data.resize(offset + text.size(), 0);
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        data[offset + index] = static_cast<std::uint8_t>(text[index]);
    }
}

// Wraps a section body (everything after the 8-byte tag + size header) in its
// tag and total size. Bodies are written with section-relative offsets.
std::vector<std::uint8_t> makeSection(std::string_view tag, std::vector<std::uint8_t> body) {
    std::vector<std::uint8_t> section(8, 0);
    putText(section, 0, tag);
    putU32(section, 4, static_cast<std::uint32_t>(body.size() + 8));
    section.insert(section.end(), body.begin(), body.end());
    return section;
}

// VTX1 with a unit quad: positions, normals and one texture coordinate array.
// The 13-entry offset table is an ordered list (the slot positions carry no
// meaning beyond the order), so the arrays are placed in the slots the format
// uses for position, normal and colour data and described by the definitions.
std::vector<std::uint8_t> makeVtx1Body() {
    constexpr std::size_t kArrayDefinitions = 0x40;
    constexpr std::size_t kPositionData = kArrayDefinitions + 3 * 0x10; // 0x70
    constexpr std::size_t kNormalData = kPositionData + 4 * 3 * 4;      // 0xA0
    constexpr std::size_t kTexCoordData = kNormalData + 4 * 3 * 4;      // 0xD0
    constexpr std::size_t kSectionSize = kTexCoordData + 4 * 2 * 4;     // 0xF0

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU32(body, 0, static_cast<std::uint32_t>(kArrayDefinitions));
    putU32(body, 0x0C - 8 + 9 * 4, static_cast<std::uint32_t>(kPositionData));
    putU32(body, 0x0C - 8 + 10 * 4, static_cast<std::uint32_t>(kNormalData));
    putU32(body, 0x0C - 8 + 11 * 4, static_cast<std::uint32_t>(kTexCoordData));

    const auto writeDefinition = [&body](std::size_t sectionOffset, std::uint32_t arrayType,
                                         std::uint32_t componentCount) {
        const std::size_t at = sectionOffset - 8;
        putU32(body, at, arrayType);
        putU32(body, at + 4, componentCount);
        putU32(body, at + 8, 4); // 4 = f32
        body[at + 0xC] = 0;      // fraction bits
    };
    writeDefinition(kArrayDefinitions + 0 * 0x10, 9, 1);  // positions, code 1 = XYZ
    writeDefinition(kArrayDefinitions + 1 * 0x10, 10, 0); // normals, code 0 = XYZ
    writeDefinition(kArrayDefinitions + 2 * 0x10, 13, 1); // texcoord 0, code 1 = UV

    const float positions[4][3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const float texcoords[4][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        for (std::size_t component = 0; component < 3; ++component) {
            putF32(body, kPositionData - 8 + (vertex * 3 + component) * 4, positions[vertex][component]);
            putF32(body, kNormalData - 8 + (vertex * 3 + component) * 4, component == 2 ? 1.0F : 0.0F);
        }
        for (std::size_t component = 0; component < 2; ++component) {
            putF32(body, kTexCoordData - 8 + (vertex * 2 + component) * 4, texcoords[vertex][component]);
        }
    }
    return body;
}

// INF1: a root joint with one material and one shape drawn inside it, then the
// terminator. The 0x01/0x02 opcodes exercise the hierarchy stack.
std::vector<std::uint8_t> makeInf1Body() {
    constexpr std::size_t kNodeTable = 0x18;
    const std::uint16_t nodes[6][2] = {{0x10, 0}, {0x01, 0}, {0x11, 0}, {0x12, 0}, {0x02, 0}, {0x00, 0}};
    std::vector<std::uint8_t> body(kNodeTable - 8 + 6 * 4, 0);
    putU32(body, 0x10 - 8, 4);          // vertex count
    putU32(body, 0x14 - 8, kNodeTable); // hierarchy data offset
    for (std::size_t index = 0; index < 6; ++index) {
        putU16(body, kNodeTable - 8 + index * 4, nodes[index][0]);
        putU16(body, kNodeTable - 8 + index * 4 + 2, nodes[index][1]);
    }
    return body;
}

// JNT1: a single joint named "Root" with a scale of one.
std::vector<std::uint8_t> makeJnt1Body() {
    constexpr std::size_t kRemapTable = 0x18;
    constexpr std::size_t kJointData = 0x20;
    constexpr std::size_t kNameTable = 0x60;
    constexpr std::size_t kSectionSize = 0x70;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // joint count
    putU32(body, 0x0C - 8, kJointData);
    putU32(body, 0x10 - 8, kRemapTable);
    putU32(body, 0x14 - 8, kNameTable);
    putU16(body, kRemapTable - 8, 0); // remap[0] -> joint record 0
    putF32(body, kJointData - 8 + 4, 1.0F);
    putF32(body, kJointData - 8 + 8, 1.0F);
    putF32(body, kJointData - 8 + 12, 1.0F);
    putU16(body, kNameTable - 8 + 4 + 2, 8); // name string offset within the table
    putText(body, kNameTable - 8 + 8, "Root");
    return body;
}

// DRW1: one unweighted draw matrix pointing at joint 0.
std::vector<std::uint8_t> makeDrw1Body() {
    std::vector<std::uint8_t> body(0x14, 0);
    putU16(body, 0, 1);
    putU32(body, 0x0C - 8, 0x14); // weighted flag array
    putU32(body, 0x10 - 8, 0x18); // matrix index array
    putU16(body, 0x18 - 8, 0);
    return body;
}

// SHP1: one batch, one packet, one quad primitive using byte indices.
std::vector<std::uint8_t> makeShp1Body() {
    constexpr std::size_t kRemapTable = 0x2C;
    constexpr std::size_t kBatchRecord = 0x30;
    constexpr std::size_t kAttributes = 0x58;
    constexpr std::size_t kMatrixData = 0x78;
    constexpr std::size_t kPacketLocations = 0x80;
    constexpr std::size_t kPacketData = 0x88;
    constexpr std::size_t kPacketSize = 3 + 4 * 3 + 1; // type + count + indices + terminator
    constexpr std::size_t kSectionSize = kPacketData + kPacketSize;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // batch count
    putU32(body, 0x0C - 8, kBatchRecord);
    putU32(body, 0x10 - 8, kRemapTable);
    putU32(body, 0x18 - 8, kAttributes);
    putU32(body, 0x1C - 8, 0); // matrix table (unused for single-matrix batches)
    putU32(body, 0x20 - 8, kPacketData);
    putU32(body, 0x24 - 8, kMatrixData);
    putU32(body, 0x28 - 8, kPacketLocations);
    putU16(body, kRemapTable - 8, 0); // remap[0] -> batch record 0

    body[kBatchRecord - 8] = 1;        // matrix type: one matrix per packet
    // Batch entry: matrix type, a pad byte, then packet count, attribute-list
    // offset, first-matrix index and first-packet index as four consecutive
    // u16s (Java readSHP1: readByte, skip(1), then four readShorts). The first
    // u16 sits directly after the pad byte -- a port that read all four one
    // slot too far decoded packetCount as 0 on real BDLs and silently dropped
    // every triangle.
    putU16(body, kBatchRecord - 8 + 2, 1); // packet count
    putU16(body, kBatchRecord - 8 + 4, 0); // attribute list offset
    putU16(body, kBatchRecord - 8 + 6, 0); // first matrix index
    putU16(body, kBatchRecord - 8 + 8, 0); // first packet index

    const std::uint32_t attributeTypes[3] = {9, 10, 13};
    for (std::size_t index = 0; index < 3; ++index) {
        putU32(body, kAttributes - 8 + index * 8, attributeTypes[index]);
        putU32(body, kAttributes - 8 + index * 8 + 4, 1); // 1 = byte indices
    }
    putU32(body, kAttributes - 8 + 3 * 8, 0xFF); // attribute terminator

    putU32(body, kPacketLocations - 8, static_cast<std::uint32_t>(kPacketSize));      // packet size
    putU32(body, kPacketLocations - 8 + 4, 0);  // packet offset within the data block

    body[kPacketData - 8] = 0x80; // GX quads
    putU16(body, kPacketData - 8 + 1, 4);
    std::size_t cursor = kPacketData - 8 + 3;
    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        for (std::size_t attribute = 0; attribute < 3; ++attribute) {
            body[cursor++] = static_cast<std::uint8_t>(vertex);
        }
    }
    body[cursor] = 0; // primitive list terminator
    return body;
}

// MAT3: one material named "mat" with a brown diffuse colour. Every index in
// the material's 0x14C record points at entry 0 of its field table.
std::vector<std::uint8_t> makeMat3Body() {
    constexpr std::size_t kSectionStart = 0x88;  // start of the init-data records
    constexpr std::size_t kRecordSize = 0x14C;
    constexpr std::size_t kRemapTable = 0x1D8;
    constexpr std::size_t kNameTable = 0x1DC;
    constexpr std::size_t kCullTable = 0x1E8;
    constexpr std::size_t kMaterialColorTable = 0x1EC;
    constexpr std::size_t kAmbientColorTable = 0x1F4;
    constexpr std::size_t kColorChannelCountTable = 0x1FC;
    constexpr std::size_t kTexGenCountTable = 0x1FD;
    constexpr std::size_t kTevStageCountTable = 0x1FE;
    constexpr std::size_t kZCompLocTable = 0x1FF;
    constexpr std::size_t kDitherTable = 0x200;
    constexpr std::size_t kZModeTable = 0x204;
    constexpr std::size_t kAlphaCompareTable = 0x208;
    constexpr std::size_t kBlendInfoTable = 0x210;
    constexpr std::size_t kColorChannelTable = 0x218;
    constexpr std::size_t kTextureIndexTable = 0x220;
    constexpr std::size_t kSectionSize = 0x230;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // material count
    putU32(body, 0x0C - 8, kSectionStart);        // InitDataTableOffset
    putU32(body, 0x10 - 8, kRemapTable);          // RemapTableOffset
    putU32(body, 0x14 - 8, kNameTable);           // NameTableOffset
    putU32(body, 0x1C - 8, kCullTable);           // CullModeInfoOffset
    putU32(body, 0x20 - 8, kMaterialColorTable);  // MaterialColorTableOffset
    putU32(body, 0x24 - 8, kColorChannelCountTable); // ColorChannelCountTableOffset
    putU32(body, 0x28 - 8, kColorChannelTable);   // ColorChannelTableOffset
    putU32(body, 0x2C - 8, kAmbientColorTable);   // AmbientColorTableOffset
    putU32(body, 0x34 - 8, kTexGenCountTable);    // TexGenCountTableOffset
    putU32(body, 0x48 - 8, kTextureIndexTable);   // TextureIndexTableOffset
    putU32(body, 0x58 - 8, kTevStageCountTable);  // TevStageCountTableOffset
    putU32(body, 0x6C - 8, kAlphaCompareTable);   // AlphaCompareTableOffset
    putU32(body, 0x70 - 8, kBlendInfoTable);      // BlendInfoTableOffset
    putU32(body, 0x74 - 8, kZModeTable);          // ZModeTableOffset
    putU32(body, 0x78 - 8, kZCompLocTable);       // ZCompLocTableOffset
    putU32(body, 0x7C - 8, kDitherTable);         // DitherTableOffset

    body[kSectionStart - 8] = 1;                    // pixel engine mode
    putU16(body, kRemapTable - 8, 0);               // remap[0] -> material record 0
    putU16(body, kNameTable - 8 + 4 + 2, 8);        // name string offset within the table
    putText(body, kNameTable - 8 + 8, "mat");

    body[kMaterialColorTable - 8 + 0] = 128;        // diffuse RGBA8
    body[kMaterialColorTable - 8 + 1] = 64;
    body[kMaterialColorTable - 8 + 2] = 32;
    body[kMaterialColorTable - 8 + 3] = 255;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        body[kAmbientColorTable - 8 + channel] = 255;
    }
    for (std::size_t entry = 0; entry < 8; ++entry) {
        putU16(body, kTextureIndexTable - 8 + entry * 2, entry == 0 ? 0 : 0xFFFF);
    }
    // Non-zero ZMode / alpha-compare / blend-info / cull entries so the MAT3
    // walk is proven to land on those tables: a shifted walk reads the same
    // zeros a sparse fixture has, which would render as "never draw, no
    // depth, no blend" just as silently as a correct walk of real zeros.
    body[kCullTable - 8] = 2;               // cull back faces
    body[kZModeTable - 8 + 0] = 1;          // depth test on
    body[kZModeTable - 8 + 1] = 3;          // GL_LEQUAL
    body[kZModeTable - 8 + 2] = 1;          // fragments update depth
    body[kAlphaCompareTable - 8 + 0] = 4;   // GREATER
    body[kAlphaCompareTable - 8 + 1] = 128; // reference 0
    body[kAlphaCompareTable - 8 + 2] = 0;   // AND merge
    body[kAlphaCompareTable - 8 + 3] = 7;   // ALWAYS
    body[kAlphaCompareTable - 8 + 4] = 0;   // reference 1
    body[kBlendInfoTable - 8 + 0] = 1;      // blend (not none/logic/subtract)
    body[kBlendInfoTable - 8 + 1] = 4;      // src: SRC_ALPHA
    body[kBlendInfoTable - 8 + 2] = 5;      // dst: ONE_MINUS_SRC_ALPHA
    body[kBlendInfoTable - 8 + 3] = 0;      // op: add
    // The record's texture-index block sits at record + 0x84 and holds one
    // short per slot; the section grows to fit it before the tables.
    constexpr std::size_t kRecordTexBlock = kSectionStart + 0x84;
    constexpr std::size_t kNewSectionSize = kRecordTexBlock + 16;
    static_assert(kNewSectionSize <= kTextureIndexTable, "record block would overlap the texture index table");
    (void)kNewSectionSize;
    for (std::size_t entry = 0; entry < 8; ++entry) {
        putU16(body, kRecordTexBlock - 8 + entry * 2, entry == 0 ? 0 : 0xFFFF);
    }
    (void)kRecordSize;
    return body;
}
std::vector<std::uint8_t> makeTex1Body() {
    constexpr std::size_t kEntries = 0x14;
    constexpr std::size_t kImage = kEntries + 32;
    constexpr std::size_t kSectionSize = kImage + 32;

    std::vector<std::uint8_t> body(kSectionSize - 8, 0);
    putU16(body, 0, 1); // texture count
    putU32(body, 0x0C - 8, kEntries);

    const std::size_t entry = kEntries - 8;
    body[entry] = 1;                                // format: I8
    putU16(body, entry + 2, 2);                     // width
    putU16(body, entry + 4, 2);                     // height
    putU32(body, entry + 28, 32);                   // image data, relative to the entry
    body[kImage - 8 + 0] = 0x80;
    body[kImage - 8 + 1] = 0x10;
    body[kImage - 8 + 8] = 0xFF;
    body[kImage - 8 + 9] = 0x00;
    return body;
}

// A complete bmd3 file with every section the reader understands.
std::vector<std::uint8_t> makeTinyBmd() {
    const std::vector<std::vector<std::uint8_t>> sections{
        makeSection("INF1", makeInf1Body()), makeSection("VTX1", makeVtx1Body()),
        makeSection("JNT1", makeJnt1Body()), makeSection("DRW1", makeDrw1Body()),
        makeSection("SHP1", makeShp1Body()), makeSection("MAT3", makeMat3Body()),
        makeSection("TEX1", makeTex1Body()),
    };

    std::size_t total = 0x20;
    for (const auto& section : sections) {
        total += section.size();
    }

    std::vector<std::uint8_t> file(total, 0);
    putText(file, 0, "J3D2");
    putText(file, 4, "bmd3");
    putU32(file, 8, static_cast<std::uint32_t>(total));
    putU32(file, 0xC, static_cast<std::uint32_t>(sections.size()));
    std::size_t cursor = 0x20;
    for (const auto& section : sections) {
        std::copy(section.begin(), section.end(), file.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += section.size();
    }
    return file;
}

void testBmdParsing() {
    using whitehole::render::buildModelMesh;
    using whitehole::smg::parseBmd;

    const auto bytes = makeTinyBmd();
    const auto model = parseBmd(bytes);

    expect(model.version == "bmd3", "bmd version was not read");
    expect(model.bigEndian, "bmd byte order was not detected");
    expect(model.vertexCount == 4, "bmd vertex count was not read");
    expect(model.positions.size() == 4, "bmd position array size is wrong");
    expect(std::abs(model.positions[2].x - 1.0F) < 0.001F && std::abs(model.positions[2].y - 1.0F) < 0.001F,
           "bmd position data is wrong");
    expect(model.normals.size() == 4 && std::abs(model.normals[0].z - 1.0F) < 0.001F, "bmd normal data is wrong");
    expect(model.texcoords[0].size() == 4 && std::abs(model.texcoords[0][3].y - 1.0F) < 0.001F,
           "bmd texture coordinate data is wrong");
    expect(std::abs(model.boundsMin.x) < 0.001F && std::abs(model.boundsMax.y - 1.0F) < 0.001F,
           "bmd model bounds are wrong");

    expect(model.sceneGraph.size() == 2, "bmd scene graph size is wrong");
    expect(model.sceneGraph[0].nodeType == 1 && model.sceneGraph[0].nodeId == 0, "bmd joint node is wrong");
    expect(model.sceneGraph[1].nodeType == 0 && model.sceneGraph[1].nodeId == 0 &&
               model.sceneGraph[1].materialIndex == 0 && model.sceneGraph[1].parentIndex == 0,
           "bmd shape node is wrong");

    expect(model.joints.size() == 1 && model.joints[0].name == "Root", "bmd joint name was not read");
    expect(std::abs(model.joints[0].scale.y - 1.0F) < 0.001F, "bmd joint scale was not read");
    expect(model.matrixWeighted.size() == 1 && !model.matrixWeighted[0], "bmd draw matrix table is wrong");
    expect(model.matrixIndices.size() == 1 && model.matrixIndices[0] == 0, "bmd draw matrix index is wrong");
    expect(model.findJoint("Root") != nullptr && model.findJoint("Missing") == nullptr, "bmd joint lookup failed");

    expect(model.batches.size() == 1 && model.batches[0].packets.size() == 1, "bmd shape/packet count is wrong");
    expect(model.batches[0].packets[0].primitives.size() == 1, "bmd primitive count is wrong");
    // The fixture's packet matrix table must reach the DRW1 table: an empty
    // table (or a bad matrix id) is the drop gate that hid the old field-offset
    // bug behind healthy-looking batches.
    expect(!model.batches[0].packets[0].matrixTable.empty(), "shp1 packet has no matrix table");
    expect(model.batches[0].packets[0].matrixTable[0] < model.matrixIndices.size(),
           "shp1 packet matrix id misses the DRW1 table");
    expect(model.batches[0].packets[0].primitives.size() == 1, "bmd primitive count is wrong");
    const auto& primitive = model.batches[0].packets[0].primitives.front();
    expect(static_cast<int>(primitive.type) == 0x80, "bmd primitive type is wrong");
    expect(primitive.positionIndices.size() == 4, "bmd position index count is wrong");
    expect(primitive.positionIndices[3] == 3, "bmd position indices are wrong");
    expect(primitive.normalIndices.size() == 4 && primitive.texcoordIndices[0].size() == 4,
           "bmd attribute indices are wrong");

    expect(model.materials.size() == 1 && model.materials[0].name == "mat", "bmd material name was not read");
    const auto& diffuse = model.materials[0].diffuseColor;
    expect(std::abs(diffuse[0] - 128.0F / 255.0F) < 0.01F && std::abs(diffuse[2] - 32.0F / 255.0F) < 0.01F,
           "bmd material colour was not read");
    expect(std::abs(model.materials[0].ambientColor[1] - 1.0F) < 0.01F, "bmd ambient colour was not read");
    expect(model.materials[0].textureIndices[0] == 0, "bmd material texture index was not read");
    expect(model.materials[0].textureIndices[7] == -1, "bmd unused texture map should be -1");
    // Phase C: ZMode / alpha-compare / blend-info / cull table values must
    // land on the material -- only non-zero entries prove the record walk is
    // aligned, since a shifted walk reads the fixture's zeros just fine.
    expect(model.materials[0].cullingMode == 2, "bmd cull mode was not read");
    expect(model.materials[0].depthTest && model.materials[0].depthWrite &&
               model.materials[0].depthFunction == 3,
           "bmd zmode (test/function/write) was not read");
    expect(model.materials[0].alphaFunc0 == 4 && model.materials[0].alphaRef0 == 128 &&
               model.materials[0].alphaOp == 0 && model.materials[0].alphaFunc1 == 7 &&
               model.materials[0].alphaRef1 == 0,
           "bmd alpha compare was not read");
    expect(model.materials[0].alphaTestEnabled(),
           "a GREATER/ALWAYS pair must run a real alpha test");
    expect(model.materials[0].blendMode == 1 && model.materials[0].blendSrcFactor == 4 &&
               model.materials[0].blendDstFactor == 5 && model.materials[0].blendOp == 0,
           "bmd blend info was not read");
    expect(model.materials[0].translucent(),
           "a blend-mode material should join the translucent pass");

    expect(model.textures.size() == 1, "bmd texture count is wrong");
    expect(model.textures[0].width == 2 && model.textures[0].height == 2, "bmd texture header is wrong");
    expect(model.textures[0].base().rgba[0] == 0x80, "bmd texture pixels were not decoded");

    // Mesh building: the quad becomes two triangles carrying the material.
    const auto mesh = buildModelMesh(model);
    expect(mesh.triangles.size() == 2, "model mesh triangle count is wrong");
    expect(mesh.skippedPrimitives == 0, "model mesh dropped a triangle primitive");
    expect(mesh.droppedEmptyMatrixTable == 0 && mesh.droppedBadMatrixIndex == 0,
           "model mesh dropped the packet at the matrix gate");
    expect(std::abs(mesh.boundsMax.x - 1.0F) < 0.001F && mesh.radius > 0.0F, "model mesh bounds are wrong");
    expect(std::abs(mesh.triangles.front().color[0] - 128.0F / 255.0F) < 0.01F,
           "model mesh lost the material colour");
    expect(mesh.triangles.front().materialIndex == 0, "model mesh lost the material index");
    expect(std::abs(mesh.triangles.front().b.normal.z - 1.0F) < 0.01F, "model mesh normals are wrong");

    // Textures (Phase C): the mesh must carry the material/texture tables so
    // the renderer can bind materialIndex -> textureIndices[0] -> textures[0]
    // without keeping the parsed BmdModel alive.
    expect(mesh.materials.size() == 1 && mesh.textures.size() == 1,
           "model mesh did not carry the material/texture tables");
    expect(mesh.materials[0].textureIndices[0] == 0, "mesh material lost its texture slot");
    expect(mesh.textures[0].base().rgba[0] == 0x80, "mesh texture table lost the decoded BTI pixels");
    expect(mesh.triangles.front().translucent, "model mesh lost the translucent pass flag");

    // Multi-part merge (Phase A + C): appending a second part must shift the
    // appended triangles' material indices past dst's material table AND shift
    // the appended materials' texture indices past dst's texture table, so a
    // PlantA01 triangle still points at ITS material/texture after the
    // tables are concatenated.
    {
        auto first = buildModelMesh(model);
        auto second = buildModelMesh(model);
        whitehole::render::appendModelMesh(first, second);
        expect(first.triangles.size() == 4, "appendModelMesh did not stack the part's triangles");
        expect(first.materials.size() == 2 && first.textures.size() == 2,
               "appendModelMesh did not concatenate the material/texture tables");
        expect(first.triangles[0].materialIndex == 0, "original triangle material index changed on append");
        expect(first.triangles[2].materialIndex == 1,
               "appended triangle kept its unshifted material index");
        expect(first.materials[1].textureIndices[0] == 1,
               "appended material's texture index was not shifted past dst's texture table");
        expect(first.skippedPrimitives == 0 && first.droppedEmptyMatrixTable == 0 &&
                   first.droppedBadMatrixIndex == 0,
               "appendModelMesh corrupted the drop counters");
        // Bounds are NOT recomputed by append; the caller does it after all
        // parts are in (ModelLibrary::loadModel does exactly that).
        whitehole::render::recomputeMeshBounds(first);
        expect(first.radius > 0.0F && std::abs(first.boundsMax.x - 1.0F) < 0.001F,
               "recomputeMeshBounds after a merge produced wrong bounds");
    }

    expect(model.valid(), "bmd model should report itself as valid");

    // Skinning safety: DRW1's matrix list and EVP1's envelope list are sized
    // independently (EVP1 only lists weighted matrices), so a weighted draw
    // matrix must be resolved through the DRW1 index into the EVP1 envelope
    // list -- never by using the matrix index on the envelope list. Doing the
    // latter read out of bounds and faulted (0xC0000005) on real BDLs, so this
    // guards the exact shape of that crash: three DRW1 matrices, one envelope.
    {
        auto skinning = model;
        skinning.matrixIndices = {0, 1, 7};
        skinning.matrixWeighted = {false, true, true};
        skinning.envelopeJoints = {{0}};
        skinning.envelopeWeights = {{1.0F}};
        // Point the fixture's only packet at matrix 1, which is the weighted
        // entry -- otherwise the packet resolves matrix 0 (identity) and the
        // skinned code path is never reached.
        skinning.batches[0].packets[0].matrixTable[0] = 1;
        const auto skinned = buildModelMesh(skinning);
        // The weighted entry must resolve (DRW1 index 1 -> EVP1 envelope 0 ->
        // joint 0) rather than fault, so the fixture's quad still produces its
        // two triangles.
        expect(skinned.triangles.size() == 2, "weighted draw matrix was not resolved from the EVP1 envelope");
    }
    // A weighted matrix whose EVP1 envelope index is out of range, and a DRW1
    // index past the matrix table, both have to degrade instead of reading past
    // the end of the envelope vectors.
    {
        auto broken = model;
        broken.matrixWeighted = {false, true};
        broken.matrixIndices = {0, 9};
        broken.envelopeJoints.clear();
        broken.envelopeWeights.clear();
        broken.batches[0].packets[0].matrixTable[0] = 1;
        const auto degraded = buildModelMesh(broken);
        expect(degraded.triangles.size() == 2, "out-of-range envelope index did not fall back safely");
    }

    // Little-endian files keep the same tag bytes in the other order.
    {
        std::vector<std::uint8_t> little(0x20, 0);
        putText(little, 0, "2D3J");
        putText(little, 4, "bmd3");
        const auto parsed = parseBmd(little);
        expect(!parsed.bigEndian, "little-endian bmd was not detected");
        expect(parsed.version == "bmd3", "little-endian bmd version was not read");
    }

    const auto expectRejected = [](std::vector<std::uint8_t> data, const std::string& context) {
        bool rejected = false;
        try {
            (void)whitehole::smg::parseBmd(data);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected, context);
    };

    expectRejected({}, "empty bmd was not rejected");
    expectRejected(std::vector<std::uint8_t>(0x10, 0), "truncated bmd was not rejected");
    {
        auto badMagic = makeTinyBmd();
        putText(badMagic, 0, "J3D9");
        expectRejected(std::move(badMagic), "bmd with a bad magic was not rejected");
    }
    {
        auto badSize = makeTinyBmd();
        putU32(badSize, 0x24, 0xFFFFFFF0U); // first section size past the end of the file
        expectRejected(std::move(badSize), "bmd with a bad section size was not rejected");
    }
    {
        // Real BMD files carry sections this reader does not process yet;
        // they must be skipped, not reject the whole file.
        auto withUnknown = makeTinyBmd();
        putU32(withUnknown, 0x0C, 2);            // bump section count
        const std::size_t extra = withUnknown.size();
        withUnknown.resize(extra + 8);           // 8-byte header for the unknown section
        putText(withUnknown, static_cast<std::uint32_t>(extra), "ZZZZ"); // unknown tag
        putU32(withUnknown, static_cast<std::uint32_t>(extra + 4), 8);   // section size
        const auto parsed = whitehole::smg::parseBmd(withUnknown);
        expect(parsed.positions.size() == 4, "unknown section should not prevent geometry parse");
    }
    {
        auto truncated = makeTinyBmd();
        truncated.resize(0x40);
        expectRejected(std::move(truncated), "bmd truncated mid-section was not rejected");
    }
}

// ---------------------------------------------------------------------------
// Model library: ObjectData archive resolution -> BMD parse -> viewport mesh.
// ---------------------------------------------------------------------------

// Minimal big-endian RARC: a root directory named `rootName` holding the given
// files. Enough for the model loader to locate and read a payload; mirrors how
// real ObjectData archives address their model file as /<Root>/<Name>.bdl.
std::vector<std::uint8_t> makeMinimalRarc(std::string_view rootName,
                                          const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    // NOTE: the on-disk RARC header occupies 0x00-0x3F (file header + data
    // header), so the string table must start at 0x40. Starting it at 0x20
    // would overwrite the node/entry/string-table fields just written above.
    constexpr std::size_t kStringOffset = 0x40;
    std::size_t cursor = 0;
    const std::size_t dotOffset = cursor;
    cursor += 2; // ".\0"
    const std::size_t dotDotOffset = cursor;
    cursor += 3; // "..\0"
    const std::size_t rootOffset = cursor;
    cursor += rootName.size() + 1;
    std::vector<std::size_t> nameOffsets;
    for (const auto& [name, payload] : files) {
        nameOffsets.push_back(cursor);
        cursor += name.size() + 1;
    }
    const std::size_t stringTableSize = cursor;

    const std::size_t nodeOffset = (kStringOffset + stringTableSize + 0x1F) & ~std::size_t{0x1F};
    const std::size_t entryCount = 2 + files.size(); // "." and ".." pseudo entries
    const std::size_t entryOffset = nodeOffset + 0x10;
    std::size_t dataOffset = entryOffset + entryCount * 0x14;
    dataOffset = (dataOffset + 0x1F) & ~std::size_t{0x1F};
    std::size_t total = dataOffset;
    for (const auto& [name, payload] : files) {
        total += payload.size();
    }

    std::vector<std::uint8_t> file(total, 0);
    putText(file, 0, "RARC");
    putU32(file, 0x04, static_cast<std::uint32_t>(total));
    putU32(file, 0x08, 0x20);
    putU32(file, 0x0C, static_cast<std::uint32_t>(dataOffset - 0x20));
    putU32(file, 0x20, 1); // one root node
    putU32(file, 0x24, static_cast<std::uint32_t>(nodeOffset - 0x20));
    putU32(file, 0x28, static_cast<std::uint32_t>(entryCount));
    putU32(file, 0x2C, static_cast<std::uint32_t>(entryOffset - 0x20));
    putU32(file, 0x30, static_cast<std::uint32_t>(stringTableSize));
    putU32(file, 0x34, static_cast<std::uint32_t>(kStringOffset - 0x20));
    putU32(file, 0x38, 0);

    putText(file, kStringOffset + dotOffset, ".");
    putText(file, kStringOffset + dotDotOffset, "..");
    putText(file, kStringOffset + rootOffset, rootName);
    for (std::size_t index = 0; index < files.size(); ++index) {
        putText(file, kStringOffset + nameOffsets[index], files[index].first);
    }

    putU32(file, nodeOffset, 0x524F4F54U); // 'ROOT' magic
    putU32(file, nodeOffset + 0x04, static_cast<std::uint32_t>(rootOffset));
    putU16(file, nodeOffset + 0x08, 0); // name hash, not validated by the reader
    putU16(file, nodeOffset + 0x0A, static_cast<std::uint16_t>(entryCount));
    putU32(file, nodeOffset + 0x0C, 0); // first entry

    const auto putEntry = [&](std::size_t index, std::uint16_t type, std::uint32_t nameOffset,
                              std::uint32_t relativeData, std::uint32_t size) {
        const std::size_t entry = entryOffset + index * 0x14;
        putU16(file, entry, static_cast<std::uint16_t>(index));
        putU16(file, entry + 0x02, 0);
        putU16(file, entry + 0x04, type);
        putU16(file, entry + 0x06, static_cast<std::uint16_t>(nameOffset));
        putU32(file, entry + 0x08, relativeData);
        putU32(file, entry + 0x0C, size);
    };
    putEntry(0, 0, static_cast<std::uint32_t>(dotOffset), 0, 0);
    putEntry(1, 0, static_cast<std::uint32_t>(dotDotOffset), 0, 0);
    std::size_t relative = 0;
    for (std::size_t index = 0; index < files.size(); ++index) {
        putEntry(2 + index, 0x1100, static_cast<std::uint32_t>(nameOffsets[index]),
                 static_cast<std::uint32_t>(relative), static_cast<std::uint32_t>(files[index].second.size()));
        relative += files[index].second.size();
    }
    std::size_t payloadCursor = dataOffset;
    for (const auto& [name, payload] : files) {
        std::copy(payload.begin(), payload.end(), file.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
        payloadCursor += payload.size();
    }
    return file;
}

void writeTestFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("could not write test file: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void testModelLibrary() {
    using whitehole::render::ModelLibrary;

    TemporaryDirectory temporary;
    const auto objectData = temporary.path / "ObjectData";
    if (!std::filesystem::create_directory(objectData)) {
        throw std::runtime_error("could not create test ObjectData directory");
    }

    const auto tiny = makeTinyBmd();
    writeTestFile(objectData / "TestObj.arc", makeMinimalRarc("TestObj", {{"TestObj.bdl", tiny}}));
    writeTestFile(objectData / "TestObjLow.arc", makeMinimalRarc("TestObjLow", {{"TestObjLow.bdl", tiny}}));
    writeTestFile(objectData / "Ambiguous.arc",
                  makeMinimalRarc("Ambiguous", {{"Ambiguous.bmd", tiny}, {"Other.bmd", tiny}}));
    writeTestFile(objectData / "OddLayout.arc", makeMinimalRarc("OddLayout", {{"SomeModel.bmd", tiny}}));

    whitehole::io::DirectoryFilesystem workspace(temporary.path);
    ModelLibrary library;
    expect(!library.bound(), "unbound library must report unbound");
    expect(library.model("TestObj") == nullptr, "unbound library must return no model");
    library.bind(&workspace);
    expect(library.bound(), "bound library must report bound");

    // A missing archive resolves to nothing and stays a cached miss.
    expect(library.archiveNameFor("Missing").empty(), "missing archive should not resolve");
    expect(library.model("Missing") == nullptr, "missing archive should produce no model");
    expect(library.missingCount() == 1, "missing lookup was not counted");

    // The model name resolves to the archive, parses, and builds the same mesh
    // a direct BMD parse would.
    expect(library.archiveNameFor("TestObj") == "TestObj.arc", "archive name resolution is wrong");
    const auto mesh = library.model("TestObj");
    expect(mesh != nullptr, "TestObj model did not load");
    const auto expected = whitehole::render::buildModelMesh(whitehole::smg::parseBmd(tiny));
    expect(mesh->triangles.size() == expected.triangles.size(), "loaded model triangle count is wrong");
    expect(!mesh->empty(), "loaded model mesh is empty");
    expect(library.loadedCount() == 1 && library.missingCount() == 1, "model counters are wrong");
    // Caching: the same mesh object is returned for repeated lookups.
    expect(library.model("TestObj") == mesh, "model cache did not reuse the mesh");
    expect(library.loadedCount() == 1 && library.missingCount() == 1,
           "cached lookups must not inflate the counters");

    // Archives with more than one model file are ambiguous and refused.
    expect(library.model("Ambiguous") == nullptr, "ambiguous archive must not produce a model");
    // A single model file in an unexpected layout still resolves.
    const auto odd = library.model("OddLayout");
    expect(odd != nullptr, "single-model fallback layout did not load");
    expect(odd->triangles.size() == expected.triangles.size(), "fallback layout mesh is wrong");

    // The low-poly setting switches to the Low archive when it exists.
    library.setLowPoly(true);
    expect(library.archiveNameFor("TestObj") == "TestObjLow.arc", "low-poly archive resolution is wrong");
    const auto lowMesh = library.model("TestObj");
    expect(lowMesh != nullptr && lowMesh != mesh, "low-poly switch did not reload the model");
    library.setLowPoly(false);
    expect(library.archiveNameFor("TestObj") == "TestObj.arc", "disabling low-poly did not restore resolution");
    const auto restoredMesh = library.model("TestObj");
    // setLowPoly() clears the cache, so the restored mesh is a fresh load of
    // the same archive: compare content, not identity.
    expect(restoredMesh != nullptr && restoredMesh != lowMesh, "disabling low-poly did not reload the model");
    expect(restoredMesh->triangles.size() == mesh->triangles.size(),
           "disabling low-poly did not restore the mesh");

    // Model name substitutions: an aliased name resolves to its target
    // archive; names without a substitution fall through unchanged.
    const auto substitutions = temporary.path / "data";
    if (!std::filesystem::create_directory(substitutions)) {
        throw std::runtime_error("could not create test data directory");
    }
    const std::string substitutionJson =
        whitehole::util::serializeJson(whitehole::util::parseJson(R"({"testalias":"TestObj"})"));
    writeTestFile(substitutions / "modelsubstitutions.json",
                  std::vector<std::uint8_t>(substitutionJson.begin(), substitutionJson.end()));
    whitehole::db::ModelSubstitutions modelSubstitutions;
    modelSubstitutions.setBaseGameRoot(temporary.path);
    modelSubstitutions.initBaseGame();
    modelSubstitutions.load();
    expect(modelSubstitutions.isLoaded(), "model substitutions did not load");

    ModelLibrary substitutedLibrary;
    substitutedLibrary.bind(&workspace);
    substitutedLibrary.setSubstitutions(&modelSubstitutions);
    expect(substitutedLibrary.archiveNameFor("TestAlias") == "TestObj.arc",
           "substituted model name did not resolve to its target archive");
    expect(substitutedLibrary.model("TestAlias") != nullptr, "substituted model name did not load");
    // Unknown names fall through to their own archive (or to nothing).
    expect(substitutedLibrary.archiveNameFor("TestObj") == "TestObj.arc", "plain name resolution broke");
    expect(substitutedLibrary.archiveNameFor("Nothing").empty(), "unknown name must not resolve");

    // The scene attaches the model and keeps the placeholder behaviour intact.
    whitehole::smg::PlacementObject modelled;
    modelled.name = "TestObj";
    modelled.kind = "obj";
    modelled.position = {50.0F, 0.0F, 0.0F};
    modelled.scale = {2.0F, 1.0F, 1.0F};
    whitehole::smg::PlacementObject plain;
    plain.name = "Missing";
    plain.kind = "obj";
    whitehole::render::ViewportScene scene;
    scene.rebuild({modelled, plain}, &library);
    expect(scene.boxes().size() == 2, "model scene dropped objects");
    expect(scene.boxes()[0].model != nullptr, "scene did not attach the game model");
    expect(scene.boxes()[0].model == restoredMesh, "scene attached the wrong mesh");
    // Modelled objects draw at the true object scale (2x here), placeholders
    // keep the 25-unit box with its minimum visual scale clamp.
    const auto modelCorner = scene.boxes()[0].world.transformPoint({1.0F, 0.0F, 0.0F});
    expect(std::abs(modelCorner.x - (50.0F + 2.0F)) < 0.01F, "model world matrix must use the true scale");
    expect(std::abs(scene.boxes()[1].halfExtents.x - 25.0F) < 0.01F, "placeholder extents changed");
    // Picking hits the model's bounding sphere.
    whitehole::render::ViewportCamera camera;
    camera.target = modelled.position;
    camera.yawRadians = 0.0F;
    camera.pitchRadians = 0.0F;
    camera.distance = 500.0F;
    expect(scene.pick(camera, 400.0F, 300.0F, 800.0F, 600.0F).has_value(), "picking missed a modelled object");
    // Rebuilding without a library keeps every object on the placeholder path.
    whitehole::render::ViewportScene plainScene;
    plainScene.rebuild({modelled});
    expect(plainScene.boxes().front().model == nullptr, "scene attached a model without a library");
}
} // namespace

// The theme is data, so it can be checked instead of eyeballed. Every contrast
// bug this file has had was invisible to the compiler and only surfaced as "I
// can't read this", so the palette now carries its own guard.
//
// Two deliberate choices keep this honest rather than mechanical. Disabled ink
// is checked on *rest* surfaces only, because ImGui never applies the hovered
// or pressed frame colours to a disabled item. Decorative accent is checked
// against the 3.0 guideline, but only on rest surfaces for the same reason:
// there is deliberately no bright-azure escape hatch for small controls.
// The D3D clear colour the shell paints its unused pixels with must track the
// palette, or the bars the light theme used to show come straight back. This
// pins that relationship so a palette edit cannot silently break it.
void testShellBackground() {
    using whitehole::app::Palette;
    using whitehole::app::Rgba;
    using whitehole::app::shellBackground;
    using whitehole::app::themePalette;

    // Rgba is a plain aggregate with no operator==, so compare the channels.
    const auto sameColor = [](const Rgba& a, const Rgba& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    for (int theme = 0; theme < 2; ++theme) {
        const bool dark = theme == 0;
        const Palette& palette = themePalette(dark);
        expect(sameColor(shellBackground(dark), palette.windowBg),
               std::string(dark ? "dark" : "light") +
                   " shellBackground must track windowBg");
        // The shell is the fallback for any pixel the UI does not paint, so it
        // has to be a real surface colour rather than an uninitialised black.
        expect(shellBackground(dark).a > 0.0F,
               std::string(dark ? "dark" : "light") + " shellBackground is transparent");
    }
}

void testThemeContrast() {
    using whitehole::app::blend;
    using whitehole::app::contrastRatio;
    using whitehole::app::kGlyphContrastMinimum;
    using whitehole::app::kTextContrastMinimum;
    using whitehole::app::Palette;
    using whitehole::app::Rgba;
    using whitehole::app::themePalette;

    struct Surface {
        const char* name;
        const Rgba* color;
    };

    for (int theme = 0; theme < 2; ++theme) {
        const bool dark = theme == 0;
        const std::string label = dark ? "dark" : "light";
        const Palette& p = themePalette(dark);

        // Body text can land anywhere, so it covers the full surface set; every
        // other role sticks to the rest surfaces it actually appears on.
        const Surface all[] = {
            {"windowBg", &p.windowBg},       {"panelBg", &p.panelBg},
            {"frameBg", &p.frameBg},         {"frameHover", &p.frameHover},
            {"frameActive", &p.frameActive}, {"header", &p.header},
            {"headerHover", &p.headerHover}, {"headerActive", &p.headerActive},
            {"tabSelected", &p.tabSelected},
        };
        const Surface rest[] = {
            {"windowBg", &p.windowBg}, {"panelBg", &p.panelBg},
            {"frameBg", &p.frameBg},   {"header", &p.header},
            {"tabSelected", &p.tabSelected},
        };

        // Collected rather than asserted one by one: expect() throws, so the
        // first failure would hide every other one in the same theme.
        std::vector<std::string> failures;
        const auto check = [&](const Rgba& fg, const Rgba& bg, double minimum,
                               const char* what, const char* where) {
            const double ratio = std::round(contrastRatio(fg, bg) * 100.0) / 100.0;
            if (ratio < minimum) {
                failures.push_back(std::string(what) + " on " + where + " is " +
                                   std::to_string(ratio) + ":1");
            }
        };

        for (const auto& surface : all) {
            check(p.text, *surface.color, kTextContrastMinimum, "body text", surface.name);
            // mix() in gui_theme.cpp already pulls hovered/pressed surfaces 25-35%
            // toward this ink, so only test it where it is painted directly; the
            // mixed backgrounds hold the same floor through the blend.
            if (surface.color == &p.windowBg || surface.color == &p.panelBg ||
                surface.color == &p.tabSelected) {
                check(p.accent, *surface.color, kGlyphContrastMinimum, "accent decoration",
                      surface.name);
            }
        }
        for (const auto& surface : rest) {
            // Decoration sits on the rest surfaces above and on the chrome surfaces
            // tested above; pressed-control accents come from the same deep tone,
            // which clears 3.0 everywhere.
            check(p.accent, *surface.color, kGlyphContrastMinimum, "accent decoration",
                  surface.name);
            check(p.textDim, *surface.color, kTextContrastMinimum, "secondary text",
                  surface.name);
            check(blend(p.textDim, *surface.color, p.disabledAlpha), *surface.color,
                  kTextContrastMinimum, "disabled text", surface.name);
            // Glyph roles: check marks, links, slider grabs.
            check(p.accentFg, *surface.color, kTextContrastMinimum, "accent glyph",
                  surface.name);
        }
        // The unsaved marker is drawn on the tab strip and in the status bar.
        check(p.unsaved, p.tabSelected, kTextContrastMinimum, "unsaved marker", "tabSelected");
        check(p.unsaved, p.windowBg, kTextContrastMinimum, "unsaved marker", "windowBg");
        check(p.unsaved, p.panelBg, kTextContrastMinimum, "unsaved marker", "panelBg");
        // Error copy is drawn in a popup or over a panel.
        check(p.error, p.panelBg, kTextContrastMinimum, "error text", "panelBg");
        check(p.error, p.windowBg, kTextContrastMinimum, "error text", "windowBg");

        std::string joined;
        for (const auto& failure : failures) {
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += failure;
        }
        expect(failures.empty(), label + " theme contrast: " + joined);

        // The palette has to actually de-emphasise, or "dim" means nothing.
        expect(contrastRatio(p.text, p.panelBg) > contrastRatio(p.textDim, p.panelBg),
               label + " theme: secondary text is not dimmer than body text");
        expect(p.disabledAlpha > 0.60F,
               label + " theme: disabledAlpha is still at ImGui's failing default");
    }
}

// ---- rails (paths) --------------------------------------------------------
// Port coverage for RailUtil: bezier maths against known values.

void testRailMath() {
    namespace rail = whitehole::smg;
    // Straight line 0 -> 400 on X with handles a third of the way along.
    rail::PathPoint start;
    start.position = {0.0F, 0.0F, 0.0F};
    start.control2 = {400.0F / 3.0F, 0.0F, 0.0F};
    rail::PathPoint end;
    end.position = {400.0F, 0.0F, 0.0F};
    end.control1 = {800.0F / 3.0F, 0.0F, 0.0F};

    const auto atZero = rail::bezierPoint(0.0F, start.position, start.control2, end.control1, end.position);
    expect(atZero.length() < 0.001F, "bezier t=0 must return the first point");
    const auto atOne = rail::bezierPoint(1.0F, start.position, start.control2, end.control1, end.position);
    expect((atOne - whitehole::math::Vec3f{400.0F, 0.0F, 0.0F}).length() < 0.001F,
           "bezier t=1 must return the last point");
    const auto atHalf = rail::bezierPoint(0.5F, start.position, start.control2, end.control1, end.position);
    expect(std::abs(atHalf.x - 200.0F) < 0.5F, "bezier midpoint is off the straight line");

    expect(std::abs(rail::pathSectionLength(start, end) - 400.0) < 1.0,
           "straight section length must match its chord");

    std::vector<rail::PathPoint> line{start, end};
    expect(std::abs(rail::pathLength(line, false) - 400.0) < 1.0, "open two-point length is wrong");
    const auto midway = rail::posAtCoord(200.0, line, false);
    expect(midway.has_value() && std::abs(midway->x - 200.0F) < 1.0F, "posAtCoord(200) must sit mid-line");
    expect(!rail::posAtCoord(401.0, line, false).has_value(), "coords past an open path must miss");
    const auto tangent = rail::dirAtCoord(100.0, line, false);
    expect(tangent.has_value() && std::abs(tangent->x - 1.0F) < 0.001F,
           "straight tangent must point along +X");

    // Closed square with coincident handles: four sides of 100.
    std::vector<rail::PathPoint> square(4);
    square[0].position = {0.0F, 0.0F, 0.0F};
    square[1].position = {100.0F, 0.0F, 0.0F};
    square[2].position = {100.0F, 100.0F, 0.0F};
    square[3].position = {0.0F, 100.0F, 0.0F};
    for (auto& point : square) {
        point.control1 = point.position;
        point.control2 = point.position;
    }
    expect(std::abs(rail::pathLength(square, true) - 400.0) < 2.0, "closed square perimeter is wrong");
    expect(std::abs(rail::pathLength(square, false) - 300.0) < 2.0, "open square length is wrong");

    // Reverse [0, 1]: positions swap and each moved point swaps its handles.
    std::vector<rail::PathPoint> three(3);
    three[0].position = {10.0F, 0.0F, 0.0F};
    three[0].control1 = {11.0F, 0.0F, 0.0F};
    three[0].control2 = {12.0F, 0.0F, 0.0F};
    three[1].position = {20.0F, 0.0F, 0.0F};
    three[1].control1 = {21.0F, 0.0F, 0.0F};
    three[1].control2 = {22.0F, 0.0F, 0.0F};
    three[2].position = {30.0F, 0.0F, 0.0F};
    expect(rail::reversePoints(three, 0, 1), "reverse of a valid range must succeed");
    expect(three[0].position.x == 20.0F && three[1].position.x == 10.0F,
           "reversed range must swap positions");
    expect(three[0].control1.x == 22.0F && three[0].control2.x == 21.0F,
           "reversed point must swap its handles");
    expect(three[2].position.x == 30.0F, "points outside the range must stay put");
    expect(!rail::reversePoints(three, 0, 99), "out-of-range reverse must fail");
}

// Load/save round trip for rails: creates a path row and a points file the
// template archive never had, so saving also exercises RarcArchive::insert.

void testPathData() {
    using namespace whitehole::smg;
    const auto templates = std::filesystem::path(WHITEHOLE_SOURCE_DIR) / "data" / "templates";
    auto stage = StageArchive::openMapFile(templates / "SMG2BigGalaxyMap.arc");
    expect(loadPaths(stage).empty(), "the template must start without rails");

    std::size_t pathTable = kNoPointTable;
    for (std::size_t index = 0; index < stage.tables().size(); ++index) {
        if (stage.tables()[index].kind == "path") {
            pathTable = index;
        }
    }
    expect(pathTable != kNoPointTable, "template CommonPathInfo did not load");

    const auto objectsBefore = stage.objects().size();

    // A brand-new rail row. Add both rows first: the second addRow() may
    // reallocate the row vector, so references are taken afterwards.
    auto& info = stage.tables()[pathTable].table;
    const auto rowIndex = info.addRow();
    const auto missingIndex = info.addRow();
    auto& row = info.rows()[rowIndex];
    auto& missing = info.rows()[missingIndex];
    info.setString(row, "name", "TestRail");
    info.setString(row, "type", "Bezier");
    info.setString(row, "closed", "OPEN");
    info.setString(row, "usage", "General");
    info.setInt(row, "l_id", 7);
    info.setInt(row, "no", 0);
    info.setInt(row, "Path_ID", -1);
    // A second rail whose points file does not exist: loading must tolerate it.
    info.setString(missing, "name", "MissingRail");
    info.setInt(missing, "l_id", 8);
    info.setInt(missing, "no", 9);

    stage.rebuildObjects();
    expect(stage.objects().size() == objectsBefore, "rail rows leaked into the placement object list");

    // A points file the template never had -- saving must insert it.
    ObjectTable points;
    points.path = pathPointFile(0);
    points.kind = "pathpoint";
    points.layer = "Common";
    ensurePathPointSchema(points.table);
    const auto addPoint = [&points](std::int16_t id, whitehole::math::Vec3f position) {
        const auto index = points.table.addRow();
        auto& row = points.table.rows()[index];
        points.table.setInt(row, "id", id);
        for (const char* set : {"pnt0", "pnt1", "pnt2"}) {
            points.table.setFloat(row, std::string(set) + "_x", position.x);
            points.table.setFloat(row, std::string(set) + "_y", position.y);
            points.table.setFloat(row, std::string(set) + "_z", position.z);
        }
        points.table.setInt(row, "point_arg0", 500); // Speed
    };
    // Deliberately out of order: loading must sort by id like Java did.
    addPoint(2, {200.0F, 0.0F, 0.0F});
    addPoint(0, {0.0F, 0.0F, 0.0F});
    addPoint(1, {100.0F, 50.0F, 0.0F});
    stage.tables().push_back(std::move(points));

    auto paths = loadPaths(stage);
    expect(paths.size() == 2, "loadPaths must see both rails");
    expect(paths[0].name == "TestRail" && paths[0].lId == 7, "rail fields did not load");
    expect(paths[0].points.size() == 3, "rail points did not load");
    expect(paths[0].points[0].id == 0 && paths[0].points[1].id == 1 && paths[0].points[2].id == 2,
           "points must sort by id");
    expect(paths[0].points[1].position.x == 100.0F && paths[0].points[1].position.y == 50.0F,
           "point coordinates did not load");
    expect(paths[0].points[0].args[0] == 500, "point_arg0 did not load");
    expect(paths[1].points.empty(), "a missing points file must yield an empty rail, not a throw");
    expect(paths[0].label() == "[7] TestRail", "rail label must match Java's toString");

    TemporaryDirectory temporary;
    const auto output = temporary.path / "WithRails.arc";
    stage.saveTo(output);

    auto reloaded = StageArchive::openMapFile(output);
    auto again = loadPaths(reloaded);
    expect(again.size() == 2, "rails did not survive the save");
    expect(again[0].points.size() == 3, "the inserted points file did not survive the save");
    expect(again[0].points[2].position.x == 200.0F, "saved point coordinates changed");
    expect(again[0].pointTableIndex != kNoPointTable, "the reloaded rail has no point table");
    // num_pnt was never set by hand: saving must have synced it to 3.
    const auto& infoAgain = reloaded.tables()[again[0].tableIndex].table;
    expect(infoAgain.getInt(infoAgain.rows()[again[0].rowIndex], "num_pnt", -1) == 3,
           "num_pnt was not synced to the point count on save");
    expect(again[1].points.empty(), "a rail with a missing file gained phantom points");
    for (const auto& object : reloaded.objects()) {
        expect(object.kind != "path" && object.kind != "pathpoint",
               "rail rows reappeared in the object list");
    }
}

// ---- viewport overlays ----------------------------------------------------
// The View menu's overlay toggles must actually produce (or omit) geometry.

void testOverlayScene() {
    using namespace whitehole::render;
    using whitehole::smg::PathPoint;
    using whitehole::smg::PlacementObject;
    using whitehole::smg::RailPath;

    PlacementObject camera;
    camera.kind = "camera";
    camera.name = "Cam";
    PlacementObject area;
    area.kind = "area";
    area.name = "Zone";
    PlacementObject gravity;
    gravity.kind = "gravity";
    gravity.name = "Planet";
    gravity.scale = {100.0F, 100.0F, 100.0F};
    PlacementObject plain;
    plain.kind = "obj";
    plain.name = "Kuribo";

    RailPath rail;
    rail.name = "Rail";
    rail.lId = 3;
    PathPoint start;
    start.position = {0.0F, 0.0F, 0.0F};
    PathPoint end;
    end.position = {400.0F, 0.0F, 0.0F};
    rail.points = {start, end};
    RailPath emptyRail; // no points: must contribute nothing, not crash
    emptyRail.lId = 4;
    const std::vector<RailPath> rails{rail, emptyRail};

    ViewportScene scene;
    OverlayFlags off;
    off.axis = off.areas = off.cameras = off.gravity = off.paths = false;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, off);
    expect(scene.overlays().empty(), "disabled overlay flags still produced geometry");
    expect(scene.railPaths().size() == 2, "the scene dropped the rail list");

    // Paths alone: one curve batch + one handles batch, nothing else.
    OverlayFlags pathsOnly;
    pathsOnly.axis = pathsOnly.areas = pathsOnly.cameras = pathsOnly.gravity = false;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, pathsOnly);
    expect(scene.overlays().size() == 2, "paths-only rebuild produced the wrong batches");

    OverlayFlags on;
    scene.rebuild({camera, area, gravity, plain}, nullptr, &rails, on);
    // axis x3 + camera + area + gravity + rail curve + rail handles = 8.
    expect(scene.overlays().size() == 8, "unexpected overlay batch count with every flag on");
    std::size_t segments = 0;
    for (const auto& batch : scene.overlays()) {
        segments += batch.segments.size();
    }
    expect(segments >= 100, "overlay geometry is suspiciously thin");

    // Rail colours are stable per l_id and differ between rails.
    expect(railPathColor(3) == railPathColor(3), "rail colour is not deterministic");
    expect(railPathColor(3) != railPathColor(4), "two rails received the same colour");
    expect((railPathColor(7) & 0xFFu) == 0xFFu, "rail colour must be opaque");
}

int main() {
    try {
        testBinaryData();
        testDirectoryFilesystem();
        testYaz0();
        testMath();
        testViewportCamera();
        testViewportScene();
        testObjectVisual();
        testHashes();
        testBcsvEndianness();
        testBcsvMutation();
        testUndoStack();
        testGizmoMath();
        testStageEditCommands();
        testObjectAuthoring();
        testGroupTransforms();
        testObjectModel();
        testValidation();
        testDocument();
        testRarcEndianness();
        testProjectArchives();
        testArchiveTableEdit();
        testNameTables();
        testStageAndGameModels();
        testBtiDecoding();
        testBmdParsing();
        testModelLibrary();
        testJsonRoundTrip();
        testSettingsRoundTrip();
        testShellBackground();
        testThemeContrast();
        testObjectDatabase();
        testObjectDatabaseV2();
        testObjectDatabaseCache();
        testRealObjectDatabase();
        testDataHolderRoundTrip();
        testDbHelpersRoundTrip();
        testRailMath();
        testPathData();
        testOverlayScene();
        std::cout << "All Whitehole native core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
