#include "systemtrayplugin.h"
#include "sni/statusnotifierwatcher.h"
#include "snitraywidget.h"

#include <QWindow>
#include <QWidget>
#include <QX11Info>

#include <xcb/xcb_icccm.h>

using namespace dtb;
using namespace dtb::systemtray;

SystemTrayPlugin::SystemTrayPlugin(QObject *parent)
    : QObject(parent),
      m_trayInter(new DBusTrayManager(this)),
      m_trayApplet(new TrayApplet),
      m_containerSettings(new QSettings("deepin", "dde-dock-tray"))
{
    m_trayApplet->setObjectName("sys-tray");
}

const QString SystemTrayPlugin::pluginName() const
{
    return "System-tray";
}

void SystemTrayPlugin::init(PluginProxyInterface *proxyInter)
{
    m_proxyInter = proxyInter;

    if (m_containerSettings->value("enable", false).toBool()) {
        return;
    }

    m_sniWatcher = new StatusNotifierWatcher(this);

    QDBusConnection dbusConn = QDBusConnection::sessionBus();
    const QString &host = QString("org.kde.StatusNotifierHost-") + QString::number(qApp->applicationPid());
    dbusConn.registerService(host);
    dbusConn.registerObject("/StatusNotifierHost", this);
    m_sniWatcher->RegisterStatusNotifierHost(host);

    m_proxyInter->addItem(this, "system-tray");

    connect(m_sniWatcher, &StatusNotifierWatcher::StatusNotifierItemRegistered, this, &SystemTrayPlugin::sniItemsChanged);
    connect(m_sniWatcher, &StatusNotifierWatcher::StatusNotifierItemUnregistered, this, &SystemTrayPlugin::sniItemsChanged);

    connect(m_trayInter, &DBusTrayManager::TrayIconsChanged, this, &SystemTrayPlugin::trayListChanged);
    connect(m_trayInter, &DBusTrayManager::Changed, this, &SystemTrayPlugin::trayChanged);

    m_trayInter->Manage();

    QTimer::singleShot(1, this, &SystemTrayPlugin::trayListChanged);
    QTimer::singleShot(2, this, &SystemTrayPlugin::sniItemsChanged);
}

QWidget *SystemTrayPlugin::itemWidget(const QString &itemKey)
{
    Q_UNUSED(itemKey);

    return m_trayApplet;
}

bool SystemTrayPlugin::itemAllowContainer(const QString &itemKey)
{
    Q_UNUSED(itemKey);

    return true;
}

bool SystemTrayPlugin::itemIsInContainer(const QString &itemKey)
{
    const QString widKey = getWindowClass(itemKey.toInt());
    if (widKey.isEmpty())
        return false;

    return m_containerSettings->value(widKey, false).toBool();
}

int SystemTrayPlugin::itemSortKey(const QString &itemKey)
{
    Q_UNUSED(itemKey);

    return 0;
}

void SystemTrayPlugin::setItemIsInContainer(const QString &itemKey, const bool container)
{
    m_containerSettings->setValue(getWindowClass(itemKey.toInt()), container);
}

void SystemTrayPlugin::setDefaultColor(PluginProxyInterface::DefaultColor color)
{
    Q_UNUSED(color);
}

void SystemTrayPlugin::updateTipsContent()
{
    auto trayList = m_trayList.values();

    m_trayApplet->clear();
    m_trayApplet->addWidgets(trayList);
}

const QString SystemTrayPlugin::getWindowClass(quint32 winId)
{
    auto *connection = QX11Info::connection();

    auto *reply = new xcb_icccm_get_wm_class_reply_t;
    auto *error = new xcb_generic_error_t;
    auto cookie = xcb_icccm_get_wm_class(connection, winId);
    auto result = xcb_icccm_get_wm_class_reply(connection, cookie, reply, &error);

    QString ret;
    if (result == 1)
    {
        ret = QString("%1-%2").arg(reply->class_name).arg(reply->instance_name);
        xcb_icccm_get_wm_class_reply_wipe(reply);
    }

    delete reply;
    delete error;

    return ret;
}

void SystemTrayPlugin::trayListChanged()
{
    // sleep some times wait dock add icons;

    QEventLoop loop;
    QTimer::singleShot(800, &loop, &QEventLoop::quit);
    loop.exec();

    QList<quint32> winidList = m_trayInter->trayIcons();
    QStringList trayList;

    for (auto winid : winidList) {
        trayList << XWindowTrayWidget::toTrayWidgetId(winid);
    }

    for (auto tray : m_trayList.keys())
        if (!trayList.contains(tray) && XWindowTrayWidget::isWinIdKey(tray))
            trayRemoved(tray);

    for (auto tray : trayList)
        trayAdded(tray);
}

void SystemTrayPlugin::trayAdded(const QString &itemKey)
{
    if (m_trayList.contains(itemKey)) {
        return;
    }

    AbstractTrayWidget *trayWidget = nullptr;

    if (XWindowTrayWidget::isWinIdKey(itemKey)) {
        auto winId = XWindowTrayWidget::toWinId(itemKey);
        getWindowClass(winId);
        trayWidget = new XWindowTrayWidget(winId);
    }
    else if (SNITrayWidget::isSNIKey(itemKey)) {
        const QString &sniServicePath = SNITrayWidget::toSNIServicePath(itemKey);
        trayWidget = new SNITrayWidget(sniServicePath);
        connect(trayWidget, &AbstractTrayWidget::iconChanged, this, &SystemTrayPlugin::sniItemIconChanged);
    }

    if (trayWidget) {
        m_trayList[itemKey] = trayWidget;
        m_trayApplet->addWidget(trayWidget);
    }
}

void SystemTrayPlugin::trayRemoved(const QString &itemKey)
{
    if (!m_trayList.contains(itemKey))
        return;

    AbstractTrayWidget *widget = m_trayList[itemKey];

    m_trayList.remove(itemKey);
    m_trayApplet->trayWidgetRemoved(widget);
    widget->deleteLater();

    updateTipsContent();
}

void SystemTrayPlugin::trayChanged(quint32 winId)
{
    const QString &itemKey = XWindowTrayWidget::toTrayWidgetId(winId);
    if (!m_trayList.contains(itemKey))
        return;

    m_trayList[itemKey]->updateIcon();
}

void SystemTrayPlugin::sniItemsChanged()
{
    const QStringList &itemServicePaths = m_sniWatcher->RegisteredStatusNotifierItems();
    QStringList sinTrayKeyList;

    for (auto item : itemServicePaths) {
        sinTrayKeyList << SNITrayWidget::toSNIKey(item);
    }

    for (auto itemKey : m_trayList.keys()) {
        if (!sinTrayKeyList.contains(itemKey) && SNITrayWidget::isSNIKey(itemKey)) {
            trayRemoved(itemKey);
        }
    }

    for (auto tray : sinTrayKeyList) {
        trayAdded(tray);
    }
}

void SystemTrayPlugin::sniItemIconChanged()
{
    AbstractTrayWidget *trayWidget = static_cast<AbstractTrayWidget *>(sender());
    if (!m_trayList.values().contains(trayWidget)) {
        return;
    }
}
