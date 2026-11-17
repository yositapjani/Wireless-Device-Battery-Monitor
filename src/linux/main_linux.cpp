// Linux entry point: a StatusNotifierItem (system tray) app built with GTK4/GIO.
//
// Kept separate from src/main.cpp (Win32 tray + taskbar widget) so the Windows build is not
// touched. The polling logic and all vendor drivers are shared; only the presentation layer and
// settings storage differ.
#include "../platform_compat.h"
#include "../devices.h"
#include "../hid.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>
#include <unistd.h>

namespace {

const char* kAppId = "com.github.krisgfx9.WirelessBattery";
const char* kSniPath = "/org/kde/StatusNotifierItem";
const char* kMenuPath = "/Menu";
constexpr int kPollSeconds = 60;
constexpr int kLowBattery = 15;
constexpr int kIconSize = 64;

struct MenuNode {
    int id = 0;
    int parent = 0;
    std::string label;
    std::string action;
    bool separator = false;
    bool toggle = false;
    bool checked = false;
};

struct State {
    GtkApplication* app = nullptr;
    GDBusConnection* conn = nullptr;
    GVariant* pixmap = nullptr;           // a(iiay), cached for SNI property reads
    std::string tooltip = "Wireless+ Battery";
    std::vector<DeviceStatus> devices;
    std::vector<std::string> deviceActions;
    std::vector<MenuNode> menuNodes;  // com.canonical.dbusmenu tree (id 0 is the root)
    int menuRevision = 0;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<DeviceStatus> pending;
    std::atomic<bool> stop{ false };
    bool wake = false;
    std::set<std::wstring> hidden;
    bool hideLastKnown = false;
    bool autostart = false;
};
State g;

// ---- settings (GKeyFile under $XDG_CONFIG_HOME) ------------------------------------------------

std::string ConfigPath() {
    return std::string(g_get_user_config_dir()) + "/wireless-battery/config.ini";
}

void LoadSettings() {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, ConfigPath().c_str(), G_KEY_FILE_NONE, nullptr);
    g_autofree gchar** hidden = g_key_file_get_string_list(kf, "general", "hidden", nullptr, nullptr);
    if (hidden) for (int i = 0; hidden[i]; i++) g.hidden.insert(Utf8ToWide(hidden[i]));
    g.hideLastKnown = g_key_file_get_boolean(kf, "general", "hide-last-known", nullptr);
    g.autostart = g_key_file_get_boolean(kf, "general", "autostart", nullptr);
}

void SaveSettings() {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, ConfigPath().c_str(), G_KEY_FILE_NONE, nullptr);
    std::vector<std::string> items;
    for (const auto& k : g.hidden) items.push_back(WideToUtf8(k));
    std::vector<const char*> ptrs;
    for (const auto& s : items) ptrs.push_back(s.c_str());
    if (ptrs.empty()) g_key_file_remove_key(kf, "general", "hidden", nullptr);
    else g_key_file_set_string_list(kf, "general", "hidden", ptrs.data(), ptrs.size());
    g_key_file_set_boolean(kf, "general", "hide-last-known", g.hideLastKnown);
    g_key_file_set_boolean(kf, "general", "autostart", g.autostart);
    std::string path = ConfigPath();
    g_mkdir_with_parents(g_path_get_dirname(path.c_str()), 0700);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
}

std::string DesktopPath() {
    return std::string(g_get_user_config_dir()) + "/autostart/wireless-battery.desktop";
}

void ApplyAutostart(bool on) {
    std::string path = DesktopPath();
    if (!on) { unlink(path.c_str()); return; }
    g_mkdir_with_parents(g_path_get_dirname(path.c_str()), 0700);
    g_autofree gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    if (!exe) return;
    std::string content =
        "[Desktop Entry]\nType=Application\nName=Wireless+ Battery\n"
        "Comment=Show battery levels of wireless devices in the tray\n"
        "Exec=\"" + std::string(exe) + "\"\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n";
    g_file_set_contents(path.c_str(), content.c_str(), (gssize)content.size(), nullptr);
}

// ---- icon rendering (Cairo -> SNI pixmap) ------------------------------------------------------

GVariant* RenderPixmap(int pct, bool charging, bool dim, bool low) {
    const int S = kIconSize;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    std::string text = pct < 0 ? "?" : std::string(pct >= 100 ? "99" : std::to_string(pct));
    double r = low && !charging ? 0.95 : (charging ? 0.25 : 0.92);
    double g = low && !charging ? 0.25 : (charging ? 0.70 : 0.92);
    double b = low && !charging ? 0.28 : (charging ? 1.00 : 0.92);
    double alpha = dim ? 0.45 : 1.0;

    double fontSize = text.size() >= 3 ? 28 : (text.size() == 2 ? 38 : 46);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fontSize);
    cairo_text_extents_t ext{};
    cairo_text_extents(cr, text.c_str(), &ext);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_move_to(cr, (S - ext.width) / 2 - ext.x_bearing, S * 0.48 - (ext.height / 2 + ext.y_bearing));
    cairo_show_text(cr, text.c_str());

    // Level bar.
    int barH = 7, barY = S - barH - 2;
    int fill = pct < 0 ? 0 : std::max(1, (pct * (S - 8)) / 100);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_rectangle(cr, 4, barY, S - 8, barH);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, charging ? 0.2 : (pct <= kLowBattery ? 0.95 : 0.2),
                          charging ? 0.7 : (pct <= kLowBattery ? 0.25 : 0.8),
                          charging ? 1.0 : (pct <= kLowBattery ? 0.3 : 0.45), alpha);
    cairo_rectangle(cr, 4, barY, fill, barH);
    cairo_fill(cr);
    cairo_destroy(cr);

    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    GByteArray* bytes = g_byte_array_new();
    for (int y = 0; y < S; y++) {
        const guint32* row = (const guint32*)(data + y * stride);
        for (int x = 0; x < S; x++) {
            guint32 p = row[x];  // native-endian ARGB32
            guint8 a = (p >> 24) & 0xFF, rr = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, bb = p & 0xFF;
            guint8 px[4] = { a, rr, gg, bb };  // SNI wants A,R,G,B bytes
            g_byte_array_append(bytes, px, 4);
        }
    }
    cairo_surface_destroy(surface);

    GVariant* flat = g_variant_new_from_data(
        G_VARIANT_TYPE("ay"), bytes->data, bytes->len, TRUE,
        +[](gpointer p) { g_byte_array_unref(static_cast<GByteArray*>(p)); }, bytes);
    GVariant* item = g_variant_new("(ii@ay)", S, S, flat);
    GVariant* arr = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), &item, 1);
    return g_variant_ref_sink(arr);
}

// ---- presentation state -----------------------------------------------------------------------

std::wstring StatusText(const DeviceStatus& s) {
    if (!s.connected) return L"off / not connected";
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else t = L"unknown";
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += L", charging";
    if (s.asleep) t += L", asleep";
    return t;
}

bool IsVisible(const DeviceStatus& s) {
    if (g.hidden.count(s.key)) return false;
    if (g.hideLastKnown && (s.asleep || (s.percent < 0))) return false;
    return true;
}

int KindOrder(DeviceKind k) {
    switch (k) {
        case DeviceKind::Headset: return 0;
        case DeviceKind::Mouse: return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad: return 3;
        case DeviceKind::Phone: return 4;
        default: return 5;
    }
}

void EmitSignal(const char* name) {
    if (!g.conn) return;
    g_dbus_connection_emit_signal(g.conn, nullptr, kSniPath, "org.kde.StatusNotifierItem", name, nullptr, nullptr);
}

// ---- com.canonical.dbusmenu -------------------------------------------------------------------
// StatusNotifierItem hosts (KDE Plasma, AppIndicator on GNOME) read the context menu through this
// interface at the object path given by the SNI "Menu" property. GIO's GMenu export uses
// org.gtk.Menus instead, so implement the small subset dbusmenu clients actually use.

const MenuNode* FindNode(int id) {
    if (id == 0) return nullptr;  // root
    if (id < 0 || (size_t)id > g.menuNodes.size()) return nullptr;
    return &g.menuNodes[id - 1];
}

void AddMenuProperties(GVariantBuilder& props, const MenuNode* n) {
    if (!n) {
        g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
        return;
    }
    if (n->separator) {
        g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
        return;
    }
    g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(n->label.c_str()));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string(n->toggle ? "checkbox" : "standard"));
    if (n->toggle)
        g_variant_builder_add(&props, "{sv}", "toggle-state", g_variant_new_int32(n->checked ? 1 : 0));
}

GVariant* BuildLayout(int id, int depth) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    AddMenuProperties(props, FindNode(id));
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (depth != 0) {
        for (const auto& n : g.menuNodes)
            if (n.parent == id)
                g_variant_builder_add(&children, "v", BuildLayout(n.id, depth > 0 ? depth - 1 : -1));
    }
    GVariant* layout = g_variant_new("(ia{sv}av)", id, &props, &children);
    g_variant_builder_clear(&props);
    g_variant_builder_clear(&children);
    return g_variant_ref_sink(layout);
}

void AddMenuNode(int parent, const std::string& label, const std::string& action,
                 bool separator = false, bool toggle = false, bool checked = false) {
    MenuNode n;
    n.id = (int)g.menuNodes.size() + 1;
    n.parent = parent;
    n.label = label;
    n.action = action;
    n.separator = separator;
    n.toggle = toggle;
    n.checked = checked;
    g.menuNodes.push_back(std::move(n));
}

void RebuildMenu() {
    if (!g.app) return;
    for (const auto& name : g.deviceActions)
        g_action_map_remove_action(G_ACTION_MAP(g.app), name.c_str());
    g.deviceActions.clear();
    g.menuNodes.clear();

    std::vector<const DeviceStatus*> list;
    for (const auto& s : g.devices) if (IsVisible(s)) list.push_back(&s);
    std::sort(list.begin(), list.end(), [](const DeviceStatus* a, const DeviceStatus* b) {
        int ka = KindOrder(a->kind), kb = KindOrder(b->kind);
        return ka != kb ? ka < kb : a->name < b->name;
    });
    int shown = 0;
    for (const auto* s : list) {
        std::string label = WideToUtf8(s->name) + ": " + WideToUtf8(StatusText(*s));
        std::string action = "device-" + std::to_string(shown);
        AddMenuNode(0, label, action);
        g.deviceActions.push_back(action);
        shown++;
    }
    if (!shown) AddMenuNode(0, "No devices with battery info found", "");
    AddMenuNode(0, "", "", true);
    AddMenuNode(0, "Refresh now", "refresh");
    AddMenuNode(0, g.hideLastKnown ? "Hide last known / asleep devices (on)"
                                  : "Hide last known / asleep devices (off)",
                "toggle-last-known", false, true, g.hideLastKnown);
    AddMenuNode(0, g.autostart ? "Start at login (on)" : "Start at login (off)",
                "toggle-autostart", false, true, g.autostart);
    AddMenuNode(0, "", "", true);
    AddMenuNode(0, "Quit", "quit");

    g.menuRevision++;
    if (g.conn)
        g_dbus_connection_emit_signal(g.conn, nullptr, kMenuPath, "com.canonical.dbusmenu",
                                      "LayoutUpdated", g_variant_new("(ii)", g.menuRevision, 0), nullptr);
}

void RecomputePresentation() {
    int pct = -1;
    bool charging = false, dim = false, low = false, any = false;
    std::string tip = "Wireless+ Battery";
    for (const auto& s : g.devices) {
        if (!IsVisible(s)) continue;
        any = true;
        if (s.percent >= 0) pct = s.percent;
        else dim = true;
        charging |= s.charging;
        if (s.percent >= 0 && s.percent <= kLowBattery && !s.charging) low = true;
        if (tip != "Wireless+ Battery") tip += "\n";
        else tip = "";
        tip += WideToUtf8(s.name) + ": " + WideToUtf8(StatusText(s));
    }
    if (!any) { pct = -1; tip = "Wireless+ Battery\nNo devices with battery info found"; }
    if (g.pixmap) g_variant_unref(g.pixmap);
    g.pixmap = RenderPixmap(pct, charging, dim, low);
    g.tooltip = tip;
    RebuildMenu();
    EmitSignal("NewIcon");
    EmitSignal("NewToolTip");
    EmitSignal("NewStatus");
}

void ApplyResults() {
    {
        std::lock_guard<std::mutex> lock(g.mu);
        g.devices.swap(g.pending);
    }
    RecomputePresentation();
}

// ---- SNI D-Bus interfaces ---------------------------------------------------------------------

static const char* kSniXml =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/><signal name='NewStatus'/>"
    "</interface></node>";

static const char* kAppXml =
    "<node><interface name='com.github.krisgfx9.WirelessBattery'>"
    "<method name='Quit'/>"
    "</interface></node>";

static const char* kMenuXml =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<method name='GetLayout'>"
    "<arg type='i' name='parentId' direction='in'/>"
    "<arg type='i' name='recursionDepth' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "</method>"
    "<method name='GetGroupProperties'>"
    "<arg type='ai' name='ids' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='a(ia{sv})' name='properties' direction='out'/>"
    "</method>"
    "<method name='GetProperty'>"
    "<arg type='i' name='id' direction='in'/>"
    "<arg type='s' name='name' direction='in'/>"
    "<arg type='v' name='value' direction='out'/>"
    "</method>"
    "<method name='Event'>"
    "<arg type='i' name='id' direction='in'/>"
    "<arg type='s' name='eventId' direction='in'/>"
    "<arg type='v' name='data' direction='in'/>"
    "</method>"
    "<method name='AboutToShow'><arg type='i' name='id' direction='in'/>"
    "<arg type='b' name='needUpdate' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg type='i' name='revision'/>"
    "<arg type='i' name='parent'/></signal>"
    "<property name='Revision' type='i' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "</interface></node>";

static GVariant* sni_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                  const gchar* name, GError**, gpointer) {
    if (!strcmp(name, "Id")) return g_variant_new_string("wireless-battery");
    if (!strcmp(name, "Category")) return g_variant_new_string("SystemServices");
    if (!strcmp(name, "Title")) return g_variant_new_string("Wireless+ Battery");
    if (!strcmp(name, "IconName")) return g_variant_new_string("battery-good-symbolic");
    if (!strcmp(name, "IconPixmap"))
        return g.pixmap ? g_variant_ref(g.pixmap) : RenderPixmap(-1, false, true, false);
    if (!strcmp(name, "ToolTip")) {
        GVariant* px = g.pixmap ? g_variant_ref(g.pixmap) : RenderPixmap(-1, false, true, false);
        return g_variant_new("(s@a(iiay)ss)", "", px, "Wireless+ Battery", g.tooltip.c_str());
    }
    if (!strcmp(name, "Menu")) return g_variant_new_object_path(kMenuPath);
    if (!strcmp(name, "ItemIsMenu")) return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "Status")) {
        bool low = false;
        for (const auto& s : g.devices) low |= (s.percent >= 0 && s.percent <= kLowBattery && !s.charging);
        return g_variant_new_string(low ? "NeedsAttention" : (g.devices.empty() ? "Passive" : "Active"));
    }
    return nullptr;
}

static void app_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                            GVariant*, GDBusMethodInvocation* inv, gpointer) {
    if (!strcmp(method, "Quit")) {
        g_dbus_method_invocation_return_value(inv, nullptr);
        g_application_quit(G_APPLICATION(g.app));
    } else g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

static void dbusmenu_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                 const gchar* method, GVariant* params,
                                 GDBusMethodInvocation* inv, gpointer) {
    if (!strcmp(method, "GetLayout")) {
        int parent = 0, depth = 0;
        GVariant* names = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        GVariantBuilder ret;
        g_variant_builder_init(&ret, G_VARIANT_TYPE("((ia{sv}av))"));
        g_variant_builder_add_value(&ret, BuildLayout(parent, depth));
        g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
        g_variant_unref(names);
        return;
    }
    if (!strcmp(method, "GetGroupProperties")) {
        GVariant* ids = nullptr, *names = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &names);
        GVariantBuilder ret, out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        GVariantIter it;
        g_variant_iter_init(&it, ids);
        gint id = 0;
        while (g_variant_iter_next(&it, "i", &id)) {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            AddMenuProperties(props, FindNode(id));
            g_variant_builder_add(&out, "(ia{sv})", id, &props);
            g_variant_builder_clear(&props);
        }
        g_variant_builder_init(&ret, G_VARIANT_TYPE("(a(ia{sv}))"));
        g_variant_builder_add_value(&ret, g_variant_builder_end(&out));
        g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
        g_variant_unref(ids);
        g_variant_unref(names);
        return;
    }
    if (!strcmp(method, "GetProperty")) {
        int id = 0;
        const char* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        AddMenuProperties(props, FindNode(id));
        GVariant* dict = g_variant_builder_end(&props);
        GVariant* value = g_variant_lookup_value(dict, name, nullptr);
        if (value) {
            GVariantBuilder ret;
            g_variant_builder_init(&ret, G_VARIANT_TYPE("(v)"));
            g_variant_builder_add(&ret, "v", value);
            g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
            g_variant_unref(value);
        } else {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "No such property: %s", name);
        }
        g_variant_unref(dict);
        return;
    }
    if (!strcmp(method, "Event")) {
        int id = 0;
        const char* event = nullptr;
        GVariant* data = nullptr;
        g_variant_get(params, "(i&s@v)", &id, &event, &data);
        const MenuNode* node = FindNode(id);
        if (node && !node->action.empty() && !strcmp(event, "clicked"))
            g_action_group_activate_action(G_ACTION_GROUP(g.app), node->action.c_str(), nullptr);
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
        return;
    }
    if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

static GVariant* dbusmenu_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                       const gchar* name, GError**, gpointer) {
    if (!strcmp(name, "Revision")) return g_variant_new_int32(g.menuRevision);
    if (!strcmp(name, "TextDirection")) return g_variant_new_string("ltr");
    return nullptr;
}

static GDBusInterfaceVTable sniVtable = { nullptr, sni_get_property, nullptr };
static GDBusInterfaceVTable appVtable = { app_method_call, nullptr, nullptr };
static GDBusInterfaceVTable dbusmenuVtable = { dbusmenu_method_call, dbusmenu_get_property, nullptr };

void RegisterInterfaces(GDBusConnection* conn) {
    GError* error = nullptr;
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kSniXml, &error);
    if (error) { g_warning("SNI XML: %s", error->message); g_error_free(error); return; }
    g_dbus_connection_register_object(conn, kSniPath, node->interfaces[0], &sniVtable, nullptr, nullptr, &error);
    if (error) { g_warning("Register SNI object: %s", error->message); g_error_free(error); }
    g_dbus_node_info_unref(node);

    node = g_dbus_node_info_new_for_xml(kAppXml, &error);
    if (error) { g_warning("App XML: %s", error->message); g_error_free(error); return; }
    g_dbus_connection_register_object(conn, kSniPath, node->interfaces[0], &appVtable, nullptr, nullptr, &error);
    if (error) { g_warning("Register app object: %s", error->message); g_error_free(error); }
    g_dbus_node_info_unref(node);

    node = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (error) { g_warning("Menu XML: %s", error->message); g_error_free(error); return; }
    g_dbus_connection_register_object(conn, kMenuPath, node->interfaces[0], &dbusmenuVtable, nullptr, nullptr, &error);
    if (error) { g_warning("Register menu object: %s", error->message); g_error_free(error); }
    g_dbus_node_info_unref(node);
}

void OnBusAcquired(GDBusConnection* conn, const gchar*, gpointer) {
    g.conn = conn;
    RegisterInterfaces(conn);
    // Register with the KDE/AppIndicator watcher (falls back to the freedesktop name).
    const char* watchers[] = { "org.kde.StatusNotifierWatcher", "org.freedesktop.StatusNotifierWatcher" };
    for (auto w : watchers) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn, w, "/StatusNotifierWatcher", w,
                                                      "RegisterStatusNotifierItem",
                                                      g_variant_new("(s)", kSniPath), nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply) { g_variant_unref(reply); if (error) g_error_free(error); break; }
        if (error) g_error_free(error);
    }
    RecomputePresentation();
}

// ---- actions -----------------------------------------------------------------------------------

void OnRefresh(GSimpleAction*, GVariant*, gpointer) {
    { std::lock_guard<std::mutex> lock(g.mu); g.wake = true; }
    g.cv.notify_all();
}

void OnToggleLastKnown(GSimpleAction* action, GVariant*, gpointer) {
    g.hideLastKnown = !g.hideLastKnown;
    g_simple_action_set_state(action, g_variant_new_boolean(g.hideLastKnown));
    SaveSettings();
    RecomputePresentation();
}

void OnToggleAutostart(GSimpleAction* action, GVariant*, gpointer) {
    g.autostart = !g.autostart;
    g_simple_action_set_state(action, g_variant_new_boolean(g.autostart));
    SaveSettings();
    ApplyAutostart(g.autostart);
}

void OnDevice(GSimpleAction*, GVariant*, gpointer data) {
    size_t index = (size_t)(intptr_t)data;
    std::vector<const DeviceStatus*> list;
    for (const auto& s : g.devices) if (IsVisible(s)) list.push_back(&s);
    std::sort(list.begin(), list.end(), [](const DeviceStatus* a, const DeviceStatus* b) {
        int ka = KindOrder(a->kind), kb = KindOrder(b->kind);
        return ka != kb ? ka < kb : a->name < b->name;
    });
    if (index >= list.size()) return;
    auto key = list[index]->key;
    if (!g.hidden.erase(key)) g.hidden.insert(key);
    SaveSettings();
    RecomputePresentation();
}

const GActionEntry kActions[] = {
    { "refresh", OnRefresh, nullptr, nullptr, nullptr, {} },
    { "toggle-last-known", OnToggleLastKnown, nullptr, "false", nullptr, {} },
    { "toggle-autostart", OnToggleAutostart, nullptr, "false", nullptr, {} },
};

gboolean PushResults(gpointer) {
    ApplyResults();
    return G_SOURCE_REMOVE;
}

void OnActivate(GApplication*) {
    // Tray-only app: nothing to present on activation.
}

// ---- worker ------------------------------------------------------------------------------------

void Worker() {
    while (!g.stop) {
        std::vector<DeviceStatus> result = PollAllDevices();
        {
            std::lock_guard<std::mutex> lock(g.mu);
            g.pending = std::move(result);
        }
        g_main_context_invoke(nullptr, PushResults, nullptr);
        std::unique_lock<std::mutex> lock(g.mu);
        g.cv.wait_for(lock, std::chrono::seconds(kPollSeconds), [&] { return g.stop || g.wake; });
        g.wake = false;
    }
}

// ---- CLI ---------------------------------------------------------------------------------------

int ListToConsole(bool dump) {
    if (dump) {
        std::wstring s = DumpHidCollections();
        fwrite(WideToUtf8(s).c_str(), 1, WideToUtf8(s).size(), stdout);
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) printf("No devices with battery info found.\n");
    for (const auto& s : list) {
        std::wstring line = s.name + L"  [" + s.via + L"]  " + StatusText(s);
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        line += L"   (" + s.key + L")\n";
        std::string u = WideToUtf8(line);
        fwrite(u.c_str(), 1, u.size(), stdout);
    }
    return 0;
}

int QuitRunning() {
    GError* error = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn) { fprintf(stderr, "Cannot connect to session bus: %s\n", error->message); g_error_free(error); return 1; }
    GVariant* reply = g_dbus_connection_call_sync(conn, kAppId, kSniPath,
                                                  "com.github.krisgfx9.WirelessBattery", "Quit",
                                                  nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (!reply) { fprintf(stderr, "Is Wireless+ Battery running? %s\n", error->message); g_error_free(error); }
    else g_variant_unref(reply);
    g_object_unref(conn);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) return ListToConsole(false);
        if (!strcmp(argv[i], "--dump")) return ListToConsole(true);
        if (!strcmp(argv[i], "--quit")) return QuitRunning();
    }

    LoadSettings();
    g_autoptr(GError) error = nullptr;
    if (!gtk_init_check()) { fprintf(stderr, "Cannot initialise GTK (no display?).\n"); return 1; }

    g.app = GTK_APPLICATION(gtk_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS));
    g_action_map_add_action_entries(G_ACTION_MAP(g.app), kActions, G_N_ELEMENTS(kActions), nullptr);
    // Up to 32 device toggle actions, rebound to the current device list whenever the menu is rebuilt.
    for (intptr_t i = 0; i < 32; i++) {
        std::string name = "device-" + std::to_string(i);
        GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(OnDevice), (gpointer)i);
        g_action_map_add_action(G_ACTION_MAP(g.app), G_ACTION(a));
    }

    if (!g_application_register(G_APPLICATION(g.app), nullptr, &error)) {
        fprintf(stderr, "Cannot register application (already running?): %s\n", error->message);
        return 1;
    }
    // GApplication already owns kAppId; reuse its connection for StatusNotifierItem.
    OnBusAcquired(g_application_get_dbus_connection(G_APPLICATION(g.app)), kAppId, nullptr);

    g_signal_connect(g.app, "activate", G_CALLBACK(OnActivate), nullptr);
    // No GTK window is ever created (tray only), so hold the application explicitly.
    g_application_hold(G_APPLICATION(g.app));
    std::thread worker(Worker);
    int status = g_application_run(G_APPLICATION(g.app), argc, argv);
    g.stop = true;
    g.cv.notify_all();
    worker.join();
    return status;
}
