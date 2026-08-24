#pragma once

#include <QDebug>
#include <QMap>
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <QtMath>

#include <cmath>
#include <cstring>
#include <errno.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/node/keys.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/vararg.h>
#include <spa/utils/keys.h>

/**
 * Volume status via the PipeWire native API (libpipewire-0.3).
 *
 * Unlike the D-Bus modules in this directory there is no daemon to watch
 * and no polling: the PipeWire socket is driven by Qt's own event loop
 * through a QSocketNotifier on the pw_loop fd, so every event (registry
 * global, node param change, metadata update) is dispatched in the GUI
 * thread the moment it arrives.
 *
 * How it works:
 *   * a pw_loop + pw_context are connected to the PipeWire daemon;
 *   * pw_loop_get_fd() exposes the loop's fd, which QSocketNotifier polls;
 *     whenever it becomes readable we call pw_loop_iterate(loop, 0) to
 *     dispatch the pending PipeWire events (non-blocking);
 *   * the registry is scanned for Audio/Sink nodes and for WirePlumber's
 *     "default" metadata (default.audio.sink selects the active output);
 *   * the selected sink node is bound and subscribed to SPA_PARAM_Props:
 *     volume / mute arrive as param events, instantly and event-driven;
 *   * if the daemon is not running (or dies later) we retry every few
 *     seconds, so the icon follows PipeWire automatically.
 *
 * Exposed to QML as `volumeModule` (percent / muted / sinkDescription / ...).
 * setVolume()/toggleMute()/stepVolume() also write back to the sink.
 */
class VolumeModule : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int percent READ percent NOTIFY percentChanged)          // 0..100, cubic-mapped like wpctl
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)          // linear 0.0..1.0
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString sinkName READ sinkName NOTIFY sinkNameChanged)          // node.name
    Q_PROPERTY(QString sinkDescription READ sinkDescription NOTIFY sinkDescriptionChanged)

public:
    explicit VolumeModule(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_reconnectTimer.setSingleShot(true);
        m_reconnectTimer.setInterval(5000);
        connect(&m_reconnectTimer, &QTimer::timeout, this, &VolumeModule::connectToPipeWire);

        m_pollTimer.setInterval(10000);
        connect(&m_pollTimer, &QTimer::timeout, this, &VolumeModule::pollProps);

        pw_init(nullptr, nullptr);
        connectToPipeWire();
    }

    ~VolumeModule() override
    {
        teardown();
        pw_deinit();
    }

    bool available() const { return m_available; }
    int percent() const { return m_percent; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QString sinkName() const { return m_sinkName; }
    QString sinkDescription() const { return m_sinkDescription; }

    /** Set the output volume, 0..100 (cubic-mapped to linear, like wpctl). */
    Q_INVOKABLE void setVolume(int percent)
    {
        if (!m_node)
            return;
        const float v = float(std::pow(qBound(0, percent, 100) / 100.0, 3.0));
        setNodeProps(v, m_muted);
    }

    Q_INVOKABLE void toggleMute()
    {
        if (m_node)
            setNodeProps(float(m_volume), !m_muted);
    }

    /** Relative volume step (negative = down), used by scroll/wheel handlers. */
    Q_INVOKABLE void stepVolume(int deltaPercent)
    {
        setVolume(m_percent + deltaPercent);
    }

signals:
    void availableChanged();
    void percentChanged();
    void volumeChanged();
    void mutedChanged();
    void sinkNameChanged();
    void sinkDescriptionChanged();

private:
    // ------------------------------------------------------------- connection

    void connectToPipeWire()
    {
        if (m_loop || m_core)
            return;   // already connected

        m_loop = pw_loop_new(nullptr);
        if (!m_loop) {
            qWarning() << "volume: pw_loop_new failed";
            scheduleReconnect();
            return;
        }
        m_context = pw_context_new(m_loop, nullptr, 0);
        if (!m_context) {
            qWarning() << "volume: pw_context_new failed";
            teardown();
            scheduleReconnect();
            return;
        }

        pw_properties *props = pw_properties_new(
            PW_KEY_APP_NAME, "monodybar",
            PW_KEY_APP_ID, "org.monody.monodybar",
            PW_KEY_APP_VERSION, "1.0",
            nullptr);
        m_core = pw_context_connect(m_context, props, 0);
        if (!m_core) {
            qWarning() << "volume: cannot connect to PipeWire:" << strerror(errno);
            teardown();
            scheduleReconnect();
            return;
        }
        setAvailable(true);

        // The whole trick: let Qt's event loop drive PipeWire. Watch the
        // pw_loop fd with QSocketNotifier and dispatch on every activation.
        m_notifier = new QSocketNotifier(pw_loop_get_fd(m_loop),
                                         QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] {
            if (m_loop)
                pw_loop_iterate(m_loop, 0);   // non-blocking: handle pending events
        });

        // IMPORTANT: spa_hook_list keeps a *pointer* to the events struct, so
        // the structs must outlive the listener. They are members, not locals.
        m_coreEvents = {};
        m_coreEvents.version = PW_VERSION_CORE_EVENTS;
        m_coreEvents.info = &coreInfo;
        m_coreEvents.error = &coreError;
        pw_core_add_listener(m_core, &m_coreListener, &m_coreEvents, this);

        m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
        if (!m_registry) {
            qWarning() << "volume: pw_core_get_registry failed";
            teardown();
            scheduleReconnect();
            return;
        }
        m_registryEvents = {};
        m_registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
        m_registryEvents.global = &registryGlobal;
        m_registryEvents.global_remove = &registryGlobalRemove;
        pw_registry_add_listener(m_registry, &m_registryListener, &m_registryEvents, this);

        // Order the initial dump: registry globals + metadata arrive before
        // the done event, so the first sink selection is deterministic.
        pw_core_sync(m_core, PW_ID_CORE, ++m_syncSeq);
    }

    void scheduleReconnect()
    {
        if (!m_reconnectTimer.isActive())
            m_reconnectTimer.start();
    }

    void teardown()
    {
        m_reconnectTimer.stop();
        m_pollTimer.stop();

        if (m_node) {
            spa_hook_remove(&m_nodeListener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(m_node));
            m_node = nullptr;
        }
        m_boundSinkId = 0;
        m_sinks.clear();

        if (m_metadata) {
            spa_hook_remove(&m_metadataListener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(m_metadata));
            m_metadata = nullptr;
        }
        m_metadataId = 0;

        if (m_registry) {
            spa_hook_remove(&m_registryListener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(m_registry));
            m_registry = nullptr;
        }

        if (m_core) {
            spa_hook_remove(&m_coreListener);
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        if (m_context) {
            pw_context_destroy(m_context);
            m_context = nullptr;
        }
        if (m_notifier) {
            delete m_notifier;      // remove the fd from Qt's poll() first
            m_notifier = nullptr;
        }
        if (m_loop) {
            pw_loop_destroy(m_loop);
            m_loop = nullptr;
        }

        setAvailable(false);
        setSinkName(QString());
        setSinkDescription(QString());
        setMuted(false);
        setVolumeValue(0.0);        // resets percent too
    }

    // ------------------------------------------------------------- registry

    /** One Audio/Sink global seen on the registry. */
    struct SinkInfo {
        QString name;               // node.name
        QString description;        // node.description
    };

    static void registryGlobal(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props)
    {
        Q_UNUSED(permissions);
        Q_UNUSED(version);
        auto *self = static_cast<VolumeModule *>(data);

        if (type && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char *cls = spa_dict_lookup(props, SPA_KEY_MEDIA_CLASS);
            if (!cls || strcmp(cls, "Audio/Sink") != 0)
                return;
            SinkInfo si;
            const char *name = spa_dict_lookup(props, SPA_KEY_NODE_NAME);
            const char *desc = spa_dict_lookup(props, SPA_KEY_NODE_DESCRIPTION);
            si.name = name ? QString::fromUtf8(name) : QString();
            si.description = desc ? QString::fromUtf8(desc) : si.name;
            self->m_sinks.insert(id, si);
            self->selectSink();
        } else if (type && strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            // WirePlumber's "default" metadata carries default.audio.sink
            const char *mn = spa_dict_lookup(props, "metadata.name");
            if (mn && strcmp(mn, "default") == 0 && self->m_metadataId == 0) {
                self->m_metadataId = id;
                self->bindMetadata(id);
            }
        }
    }

    static void registryGlobalRemove(void *data, uint32_t id)
    {
        auto *self = static_cast<VolumeModule *>(data);

        if (id == self->m_metadataId) {
            self->m_metadataId = 0;
            if (self->m_metadata) {
                spa_hook_remove(&self->m_metadataListener);
                pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(self->m_metadata));
                self->m_metadata = nullptr;
            }
            self->m_defaultSinkName.clear();
            self->selectSink();
        }
        if (self->m_sinks.contains(id)) {
            self->m_sinks.remove(id);
            if (id == self->m_boundSinkId) {
                self->unbindSink();
                self->selectSink();
            }
        }
    }

    // ------------------------------------------------------------- metadata

    void bindMetadata(uint32_t id)
    {
        m_metadata = static_cast<struct pw_metadata *>(
                pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Metadata,
                                 PW_VERSION_METADATA, 0));
        if (!m_metadata)
            return;
        m_metadataEvents = {};
        m_metadataEvents.version = PW_VERSION_METADATA_EVENTS;
        m_metadataEvents.property = &metadataProperty;
        pw_metadata_add_listener(m_metadata, &m_metadataListener, &m_metadataEvents, this);
        // the listener replays every entry; default.audio.sink arrives
        // (if set) and selectSink() re-runs with the correct default.
    }

    static int metadataProperty(void *data, uint32_t subject, const char *key,
                                const char *type, const char *value)
    {
        Q_UNUSED(subject);
        Q_UNUSED(type);
        auto *self = static_cast<VolumeModule *>(data);
        if (!key || strcmp(key, "default.audio.sink") != 0)
            return 0;
        self->m_defaultSinkName = value ? extractSinkName(value) : QString();
        self->selectSink();
        return 0;
    }

    /** WirePlumber stores {"name": "..."} JSON; plain node names work too. */
    static QString extractSinkName(const char *value)
    {
        const char *name = strstr(value, "\"name\":");
        if (name) {
            name = strchr(name, '"');
            if (!name)
                return {};
            ++name;                     // opening quote of the value
            const char *end = strchr(name, '"');
            if (!end)
                return {};
            return QString::fromUtf8(name, int(end - name));
        }
        return QString::fromUtf8(value);
    }

    // ------------------------------------------------------------- sinks

    void selectSink()
    {
        // prefer the sink named by the "default" metadata, fall back to any
        uint32_t want = 0;
        if (!m_defaultSinkName.isEmpty()) {
            for (auto it = m_sinks.cbegin(); it != m_sinks.cend(); ++it) {
                if (it.value().name == m_defaultSinkName) {
                    want = it.key();
                    break;
                }
            }
        }
        if (want == 0 && !m_sinks.isEmpty())
            want = m_sinks.cbegin().key();

        if (want == m_boundSinkId && m_node)
            return;
        unbindSink();
        if (want != 0)
            bindSink(want);
    }

    void bindSink(uint32_t id)
    {
        m_boundSinkId = id;
        m_node = static_cast<struct pw_node *>(pw_registry_bind(
                m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!m_node) {
            qWarning() << "volume: cannot bind sink node" << id;
            m_boundSinkId = 0;
            return;
        }
        const SinkInfo si = m_sinks.value(id);
        setSinkName(si.name);
        setSinkDescription(si.description);

        m_nodeEvents = {};
        m_nodeEvents.version = PW_VERSION_NODE_EVENTS;
        m_nodeEvents.info = &nodeInfo;
        m_nodeEvents.param = &nodeParam;
        pw_node_add_listener(m_node, &m_nodeListener, &m_nodeEvents, this);

        // Live updates: subscribe_params makes the node emit SPA_PARAM_Props
        // whenever volume/mute change. Needs X permission on the node; if it
        // is denied (restrictive policy), poll the param instead.
        uint32_t ids[] = { SPA_PARAM_Props };
        if (pw_node_subscribe_params(m_node, ids, 1) < 0) {
            m_pollTimer.setInterval(2000);
            m_pollTimer.start();
        } else {
            m_pollTimer.setInterval(10000);   // safety net only
            m_pollTimer.start();
        }
        pollProps();
    }

    void unbindSink()
    {
        if (m_node) {
            spa_hook_remove(&m_nodeListener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(m_node));
            m_node = nullptr;
        }
        m_boundSinkId = 0;
    }

    void pollProps()
    {
        if (m_node)
            pw_node_enum_params(m_node, ++m_enumSeq, SPA_PARAM_Props, 0, 1, nullptr);
    }

    // ------------------------------------------------------------- node events

    static void nodeInfo(void *data, const struct pw_node_info *info)
    {
        auto *self = static_cast<VolumeModule *>(data);
        if (!info->props)
            return;
        const char *cls = spa_dict_lookup(info->props, SPA_KEY_MEDIA_CLASS);
        if (!cls || strcmp(cls, "Audio/Sink") != 0)
            return;
        const char *name = spa_dict_lookup(info->props, SPA_KEY_NODE_NAME);
        const char *desc = spa_dict_lookup(info->props, SPA_KEY_NODE_DESCRIPTION);
        self->setSinkName(name ? QString::fromUtf8(name) : QString());
        self->setSinkDescription(desc ? QString::fromUtf8(desc) : self->m_sinkName);
    }

    static void nodeParam(void *data, int seq, uint32_t id, uint32_t index,
                          uint32_t next, const struct spa_pod *param)
    {
        Q_UNUSED(seq);
        Q_UNUSED(index);
        Q_UNUSED(next);
        auto *self = static_cast<VolumeModule *>(data);
        if (id != SPA_PARAM_Props || !param)
            return;
        self->parsePropsPod(param);
    }

    // ------------------------------------------------------------- volume

    void parsePropsPod(const struct spa_pod *pod)
    {
        // Track both the user-facing `volume` and the effective per-channel
        // volumes. Tools differ: pavucontrol-style writers set SPA_PROP_volume,
        // wpctl sets SPA_PROP_channelVolumes directly (cubed). Prefer
        // channelVolumes (cbrt) because it reflects what is really applied;
        // fall back to `volume` when the pod has no channelVolumes.
        float volume = -1.0f;   // sentinel: -1 means "not present"
        bool mute = false;
        uint32_t ch_size = 0, ch_type = 0, ch_n = 0;
        const void *ch_vals = nullptr;
        if (spa_pod_parse_object(pod, SPA_TYPE_OBJECT_Props, nullptr,
                                 SPA_PROP_volume, SPA_POD_OPT_Float(&volume),
                                 SPA_PROP_mute, SPA_POD_OPT_Bool(&mute),
                                 SPA_PROP_channelVolumes,
                                 SPA_POD_OPT_Array(&ch_size, &ch_type, &ch_n, &ch_vals)) < 0)
            return;

        setMuted(mute);
        // mute-only pods (volume/channelVolumes absent) must not reset the
        // volume to 0: only update it when either value was actually present.
        if (ch_vals && ch_n > 0) {
            const float *vals = static_cast<const float *>(ch_vals);
            float eff = 0.0f;
            for (uint32_t i = 0; i < ch_n; i++)
                eff = std::max(eff, vals[i]);
            setVolumeValue(double(eff));
        } else if (volume >= 0.0f) {
            setVolumeValue(double(volume));
        }
    }

    void setNodeProps(float volume, bool mute)
    {
        uint8_t buffer[1024];
        // `volume` is already the cubic-mapped linear gain ((percent/100)^3),
        // the same quantity WirePlumber/wpctl stores in channelVolumes. Write
        // it as-is - cubing it again would double-apply the cubic curve and
        // make 70% read back as ~34%.
        const float vols[2] = { volume, volume };
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        // builder varargs take values (not pointers): float as double, bool as int
        const struct spa_pod *param = static_cast<const struct spa_pod *>(
                spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                        SPA_PROP_volume, SPA_POD_Float(double(volume)),
                        SPA_PROP_mute, SPA_POD_Bool(int(mute)),
                        SPA_PROP_channelVolumes,
                        SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                                      SPA_N_ELEMENTS(vols), vols)));
        pw_node_set_param(m_node, SPA_PARAM_Props, 0, param);
    }

    // ------------------------------------------------------------- core events

    static void coreInfo(void *data, const struct pw_core_info *info)
    {
        auto *self = static_cast<VolumeModule *>(data);
        if (info && info->version)
            qDebug() << "volume: connected to PipeWire" << info->version;
        self->setAvailable(true);   // handshake completed
    }

    static void coreError(void *data, uint32_t id, int seq, int res, const char *message)
    {
        Q_UNUSED(seq);
        Q_UNUSED(res);
        auto *self = static_cast<VolumeModule *>(data);
        if (id != PW_ID_CORE)
            return;                 // object-level error (e.g. bind denied): ignore
        qWarning() << "volume: pipewire disconnected:" << (message ? message : "unknown");
        self->teardown();
        self->scheduleReconnect();
    }

    // ------------------------------------------------------------- state

    void setAvailable(bool v)
    {
        if (v == m_available)
            return;
        m_available = v;
        emit availableChanged();
    }

    void setVolumeValue(double v)
    {
        v = qBound(0.0, v, 1.0);
        if (v != m_volume) {
            m_volume = v;
            emit volumeChanged();
        }
        // cubic scale matches wpctl/PA: 1.0 -> 100%, 0.5 -> ~79%, ...
        const int pct = qBound(0, int(std::lround(std::cbrt(v) * 100.0)), 100);
        if (pct != m_percent) {
            m_percent = pct;
            emit percentChanged();
        }
    }

    void setMuted(bool v)
    {
        if (v == m_muted)
            return;
        m_muted = v;
        emit mutedChanged();
    }

    void setSinkName(const QString &v)
    {
        if (v == m_sinkName)
            return;
        m_sinkName = v;
        emit sinkNameChanged();
    }

    void setSinkDescription(const QString &v)
    {
        if (v == m_sinkDescription)
            return;
        m_sinkDescription = v;
        emit sinkDescriptionChanged();
    }

    // ------------------------------------------------------------- members

    struct pw_loop *m_loop = nullptr;
    struct pw_context *m_context = nullptr;
    struct pw_core *m_core = nullptr;
    struct pw_registry *m_registry = nullptr;
    struct pw_node *m_node = nullptr;
    struct pw_metadata *m_metadata = nullptr;

    struct spa_hook m_coreListener = {};
    struct spa_hook m_registryListener = {};
    struct spa_hook m_nodeListener = {};
    struct spa_hook m_metadataListener = {};

    // PipeWire keeps *pointers* to these event vtables for as long as the
    // listeners above are attached, so they must live as long as the module.
    struct pw_core_events m_coreEvents = {};
    struct pw_registry_events m_registryEvents = {};
    struct pw_node_events m_nodeEvents = {};
    struct pw_metadata_events m_metadataEvents = {};

    QSocketNotifier *m_notifier = nullptr;
    QTimer m_reconnectTimer;
    QTimer m_pollTimer;

    QMap<uint32_t, SinkInfo> m_sinks;      // registry id -> sink info
    uint32_t m_boundSinkId = 0;            // registry id of the bound node
    uint32_t m_metadataId = 0;             // registry id of the "default" metadata
    QString m_defaultSinkName;             // default.audio.sink (node.name)

    int m_syncSeq = 0;
    int m_enumSeq = 0;

    bool m_available = false;
    int m_percent = 0;
    double m_volume = 0.0;
    bool m_muted = false;
    QString m_sinkName;
    QString m_sinkDescription;
};
