
#include "login_window.h"

#include <QDebug>
#include <QTimer>
#include <QUrl>
#include <QFile>
#include <QLocale>
#include <QDBusMessage>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusConnectionInterface>
#include <QtWebChannel/QWebChannel>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineScriptCollection>

#include <DTitlebar>
#include <DWidgetUtil>

#include "sync_client.h"
#include "service/authentication_manager.h"
#include "utils/utils.h"
#include "login_view.h"
#include "login_page.h"

namespace ddc
{

class LoginWindowPrivate
{
public:
    explicit LoginWindowPrivate(LoginWindow *parent)
        : q_ptr(parent)
    {
        Q_Q(LoginWindow);
        qRegisterMetaType<AuthorizeRequest>("AuthorizeRequest");
        qRegisterMetaType<AuthorizeResponse>("AuthorizeResponse");

        QString clientID = "163296859db7ff8d72010e715ac06bdf6a2a6f87";
        QString redirectURI = "https://sync.deepinid.deepin.com/oauth/callback";
        QStringList scopes = {"base", "user:read", "sync", "dstore"};
        url = utils::authCodeURL(clientID, scopes, redirectURI, "");
        /*
        QDBusInterface activate("com.deepin.license",
                                "/com/deepin/license/Info",
                                "com.deepin.license.Info",
                                QDBusConnection::systemBus());
        authorizationState = activate.property("AuthorizationState").toUInt();
        */
        QObject::connect(&authMgr, &AuthenticationManager::requestLogin, parent, [=](const AuthorizeRequest &authReq)
        {
            // if need login,clean cookie;
            this->page->runJavaScript(
                "document.cookie.split(\";\").forEach(function(c) { document.cookie = c.replace(/^ +/, \"\").replace(/=.*/, \"=;expires=\" + new Date().toUTCString() + \";path=/\"); });");
            this->client.cleanSession();

            url = utils::authCodeURL(
                authReq.clientID,
                authReq.scopes,
                authReq.callback,
                authReq.state);
            q->load();
            q->show();
        }, Qt::QueuedConnection);

        QObject::connect(&authMgr, &AuthenticationManager::authorizeFinished, parent, [=](const AuthorizeResponse &resp)
        {
            auto clientCallback = megs.value(resp.req.clientID);
            qDebug() << resp.req.clientID << clientCallback;
            if (nullptr == clientCallback) {
                qWarning() << "empty clientID" << resp.req.clientID;
                return;
            }

            clientCallback->call(QDBus::Block, "OnAuthorized", resp.code, resp.state);
            qDebug() << "call" << clientCallback << resp.code << resp.state;

            this->hasLogin = true;
            parent->hide();
            if (!authMgr.hasRequest()) {
                parent->close();
            }
            q_ptr->windowloadingEnd = true;
        }, Qt::QueuedConnection);

        QObject::connect(&client, &SyncClient::onLogin, parent, [=](
            const QString &sessionID,
            const QString &clientID,
            const QString &code,
            const QString &state)
        {
            qDebug() << "on login";
            this->hasLogin = true;
            this->authMgr.onLogin(sessionID, clientID, code, state);
            if(activatorClientID != clientID){
                this->client.setSession();
            }
        }, Qt::QueuedConnection);

        QObject::connect(&client, &SyncClient::onCancel, parent, [=](
            const QString &clientID)
        {
            qDebug() << "onCancel";
            cancelAll();
            q_ptr->windowloadingEnd = true;
        });

        QObject::connect(&client, &SyncClient::JSIsReady, parent, [=]()
        {
            qDebug() << "JS is Ready";
            this->page->runJavaScript(
                QString("changeThemeType('%1')").arg(utils::getThemeName()));
            this->page->runJavaScript(
                QString("changeActiveColor('%1')").arg(utils::getActiveColor()));
        });
    }

    void cancelAll()
    {
        Q_Q(LoginWindow);
        authMgr.cancel();

        if (client.userInfo().value("UserID").toLongLong() <= 0) {
            for (const auto &id: megs.keys()) {
                cancel(id);
                megs.remove(id);
            }
        }

        //页面隐藏前界面刷为空白： 1. 再次打开时，避免show登录界面然后再刷新一遍的效果；2. 避免show出来时,显示之前界面中用户名密码记录，再刷新登录界面
        this->page->load(QUrl("about:blank"));
        q->hide();
    }

    void cancel(const QString &clientID)
    {
        auto clientCallback = megs.value(clientID);
        qDebug() << clientID << clientCallback;
        if (nullptr == clientCallback) {
            qWarning() << "empty clientID" << clientID;
            return;
        }

        clientCallback->call(QDBus::Block, "OnCancel");
        qDebug() << "call" << clientCallback;

    }
    QString url;
    LoginPage *page;

    SyncClient client;
    AuthenticationManager authMgr;

    bool hasLogin = false;
    QMap<QString, QDBusInterface *> megs;

    LoginWindow *q_ptr;
    Q_DECLARE_PUBLIC(LoginWindow)

    //unsigned int authorizationState;
    const QString activatorClientID = "73560e1f5fcecea6af107d7aa638e3be8b8aa97f";
};

LoginWindow::LoginWindow(QWidget *parent)
    : Dtk::Widget::DMainWindow(parent), dd_ptr(new LoginWindowPrivate(this))
{
    Q_D(LoginWindow);

    QFile scriptFile(":/qtwebchannel/qwebchannel.js");
    scriptFile.open(QIODevice::ReadOnly);
    QString apiScript = QString::fromLatin1(scriptFile.readAll());
    scriptFile.close();
    QWebEngineScript script;
    script.setSourceCode(apiScript);
    script.setName("qwebchannel.js");
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setRunsOnSubFrames(false);
    QWebEngineProfile::defaultProfile()->scripts()->insert(script);

    this->titlebar()->setTitle("");
    setWindowFlags(Qt::Dialog);

    auto flag = windowFlags();
    flag &= ~Qt::WindowMinMaxButtonsHint;
    flag |= Qt::WindowStaysOnTopHint;
    setWindowFlags(flag);
    this->titlebar()->setMenuVisible(false);
    this->titlebar()->setMenuDisabled(true);
    // TODO: workaround for old version dtk, remove as soon as possible.
    this->titlebar()->setDisableFlags(Qt::WindowSystemMenuHint);
    this->titlebar()->setBackgroundTransparent(true);

    auto machineID = d->client.machineID();
    auto *wuri = new WebUrlRequestInterceptor();
    wuri->setHeader({
                        {"X-Machine-ID", machineID.toLatin1()}
                    });
    QWebEngineProfile::defaultProfile()->setRequestInterceptor(wuri);

    auto *channel = new QWebChannel(this);
    channel->registerObject("client", &d->client);

    d->page = new LoginPage(this);
    d->page->setWebChannel(channel);

    connect(Dtk::Gui::DGuiApplicationHelper::instance(),&Dtk::Gui::DGuiApplicationHelper::themeTypeChanged,
        this, [=](Dtk::Gui::DGuiApplicationHelper::ColorType themeType) {
        Q_UNUSED(themeType)
        d->page->runJavaScript(
            QString("changeThemeType('%1')").arg(utils::getThemeName()));
    });

    QDBusConnection::sessionBus().connect(
        "com.deepin.daemon.Appearance",
        "/com/deepin/daemon/Appearance",
        "org.freedesktop.DBus.Properties",
        QLatin1String("PropertiesChanged"),
        this,
        SLOT(syncActiveColor(QString,QMap<QString,QVariant>,QStringList)));

    connect(&d->client, &SyncClient::prepareClose, this, [&]()
    {
        this->close();
        this->windowloadingEnd = true;
    });

    connect(&d->client, &SyncClient::requestHide, this, [&]()
    {
        this->hide();
        this->windowloadingEnd = true;
    });

    auto view = new LoginView(this);
    view->setPage(d->page);
    this->setCentralWidget(view);
    view->setFocus();

    connect(d->page, &QWebEnginePage::loadStarted, this, [=]()
    {
        qDebug() << "ok";
    });
    connect(d->page, &QWebEnginePage::loadFinished, this, [=](bool ok)
    {
        qDebug() << ok;
        if (!ok) {
            QNetworkConfigurationManager mgr;
            QString oauthURI = "https://login.chinauos.com";

            if (!qEnvironmentVariableIsEmpty("DEEPINID_OAUTH_URI")) {
                oauthURI = qgetenv("DEEPINID_OAUTH_URI");
            }

            if(!mgr.isOnline()) {
                d->page->load(QUrl("qrc:/web/network_error.html"));
            }else {
                QHostInfo::lookupHost(oauthURI,this,SLOT(onLookupHost(QHostInfo)));
            }
        }
        this->windowloadingEnd = true;
    });
    connect(d->page, &QWebEnginePage::loadProgress, this, [=](int progress)
    {
//        qDebug() << progress;
    });

    connect(this, &LoginWindow::loadError, this, &LoginWindow::onLoadError, Qt::QueuedConnection);

    setFixedSize(380, 550 + this->titlebar()->height());
    QTimer::singleShot(100, this, SLOT(setFocus()));
}

LoginWindow::~LoginWindow() = default;

void LoginWindow::setURL(const QString &url)
{
    Q_D(LoginWindow);
    d->url = url;
}

void LoginWindow::onLookupHost(QHostInfo host)
{
    Q_D(LoginWindow);
    if (host.error() != QHostInfo::NoError) {
        qDebug() << "Lookup failed:" << host.errorString();
        //Remote server error
        d->page->load(QUrl("qrc:/web/service_error.html"));
    }else {
        //If the local network and remote server connection is normal ,but QWebEnginePage::loadFinished is not ok.
        d->page->load(QUrl("qrc:/web/unknow_error.html"));
    }
    this->windowloadingEnd = true;
}

void LoginWindow::syncActiveColor(QString str, QMap<QString, QVariant> map, QStringList list)
{
    Q_D(LoginWindow);
    Q_UNUSED(str);
    Q_UNUSED(list);

    if(!map.contains("QtActiveColor"))
        return;

    d->page->runJavaScript(
        QString("changeActiveColor('%1')").arg(map.value("QtActiveColor").toString()));
}

void LoginWindow::load()
{
    Q_D(LoginWindow);
    qDebug() << d->url;
    d->page->load(QUrl(d->url));
}

void LoginWindow::Authorize(const QString &clientID,
                            const QStringList &scopes,
                            const QString &callback,
                            const QString &state)
{
    if(this->windowloadingEnd == true){
        this->windowloadingEnd = false;
    }else {
        return;
    }

    Q_D(LoginWindow);
    /*
    qDebug() << "d->reply:" << d->authorizationState;

    bool authorized =  AuthorizationState::Authorized == d->authorizationState ||
                    AuthorizationState::TrialAuthorized == d->authorizationState;

    if(authorized || clientID == d->activatorClientID)
    {
    */
    qDebug() << "requestAuthorize" << clientID << scopes << callback << state;
    d->authMgr.requestAuthorize(AuthorizeRequest{
                                    clientID, scopes, callback, state
                                });
    /*
    }else{
        d->page->load(QUrl("qrc:/web/authorize.html"));
        show();
    }
    */
}

void LoginWindow::onLoadError()
{
    Q_D(LoginWindow);
    qDebug() << "load error page";
    d->page->load(QUrl("qrc:/web/error.html"));
}

void LoginWindow::Register(const QString &clientID,
                           const QString &service, const QString &path,
                           const QString &interface)
{
    Q_D(LoginWindow);
    // TODO: memory leak
    qDebug() << "register" << clientID << service << path << interface;
    auto dbusIfc = new QDBusInterface(service, path, interface);

    if(!d->megs.contains(clientID)){
        d->megs.insert(clientID, dbusIfc);
    }
}

void LoginWindow::closeEvent(QCloseEvent *event)
{
    Q_D(LoginWindow);
    d->cancelAll();
    QWidget::closeEvent(event);
}

}
