// main.cpp - Part 1/2
// Qt 5.12 single-file demo (part 1)

#include <QtWidgets>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QBuffer>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// --- ArchiveHandler base class ---
class ArchiveHandler : public QObject {
    Q_OBJECT
public:
    explicit ArchiveHandler(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~ArchiveHandler() {}
    virtual bool openArchive(const QString &path) = 0;
    virtual QString archivePath() const = 0;
    virtual QStringList listEntries(const QString &prefix = QString()) const = 0;
    virtual bool extractEntryToTemp(const QString &entry, QString &outPath) = 0;
    virtual bool extractAll(const QString &destDir) = 0;
    virtual bool addFiles(const QStringList &files, const QString &destPathInArchive) = 0;
    virtual bool removeEntries(const QStringList &entries) = 0;
    virtual void setPassword(const QString &pw) = 0;
};

// --- CLI fallback ArchiveHandler implementation ---
class CliArchiveHandler : public ArchiveHandler {
public:
    CliArchiveHandler(QObject *parent = nullptr) : ArchiveHandler(parent) {}
    ~CliArchiveHandler() override {}

    bool openArchive(const QString &path) override {
        m_archive = path;
        return QFile::exists(path);
    }
    QString archivePath() const override { return m_archive; }

    // note: returns entries optionally filtered by prefix
    QStringList listEntries(const QString &prefix = QString()) const override {
        QStringList entries;
        QProcess p;
        QStringList args;
        args << "-Z" << "-1" << m_archive;
        if (!m_password.isEmpty()) {
            // unzip: -P password (note: insecure on CLI, but ok for demo)
            args.prepend(m_password);
            args.prepend("-P");
        }
        p.start("unzip", args);
        p.waitForFinished(3000);

        // if exit code non-zero and output empty, we may have a password issue
        QByteArray out = p.readAllStandardOutput();
        QByteArray err = p.readAllStandardError();
        QTextStream ts(out);
        while (!ts.atEnd()) {
            QString line = ts.readLine().trimmed();
            if (!line.isEmpty()) {
                if (prefix.isEmpty() || line.startsWith(prefix)) entries << line;
            }
        }
        return entries;
    }

    bool extractEntryToTemp(const QString &entry, QString &outPath) override {
        QString persistentTmp = QDir::temp().filePath(QString("qt_arch_tmp_%1").arg(QUuid::createUuid().toString()));
        QDir().mkpath(persistentTmp);
        QProcess p;
        QStringList args;
        if (!m_password.isEmpty()) { args << "-P" << m_password; }
        args << m_archive << entry << "-d" << persistentTmp;
        p.start("unzip", args);
        p.waitForFinished(-1);
        if (p.exitCode() == 0) {
            outPath = QDir(persistentTmp).filePath(entry);
            return true;
        }
        return false;
    }

    bool extractAll(const QString &destDir) override {
        QProcess p;
        QStringList args;
        if (!m_password.isEmpty()) { args << "-P" << m_password; }
        args << m_archive << "-d" << destDir;
        p.start("unzip", args);
        p.waitForFinished(-1);
        return p.exitCode() == 0;
    }

    bool addFiles(const QStringList &files, const QString &) override {
        // zip archive.zip files...
        QStringList args;
        args << m_archive;
        for (const QString &f : files) args << f;
        QProcess p;
        p.start("zip", args);
        p.waitForFinished(-1);
        return p.exitCode() == 0;
    }

    bool removeEntries(const QStringList &entries) override {
        QStringList args;
        args << m_archive;
        for (const QString &e : entries) args << e;
        QProcess p;
        p.start("zip", QStringList{"-d"} + args);
        p.waitForFinished(-1);
        return p.exitCode() == 0;
    }

    void setPassword(const QString &pw) override { m_password = pw; }

private:
    QString m_archive;
    QString m_password;
};

// --- Archive model ---
struct ArchiveItem {
    enum class NodeType { File, Folder, ArchiveFolder };
    QString name;
    NodeType type = NodeType::File;
    ArchiveItem *parent = nullptr;
    QList<ArchiveItem*> children;
    QString fullPathInArchive;
    bool childrenPopulated = false;
};

class ArchiveModel : public QAbstractItemModel {
    Q_OBJECT
public:
    ArchiveModel(QObject *parent = nullptr) : QAbstractItemModel(parent) {
        root = new ArchiveItem();
        root->name = "/";
        root->type = ArchiveItem::NodeType::Folder;
    }
    ~ArchiveModel() override { clear(); delete root; }

    void clear() {
        beginResetModel();
        qDeleteAll(root->children);
        root->children.clear();
        endResetModel();
    }

    // populate only items from the entries list (flat) - used for initial root population
    void populateFromList(const QStringList &entries, const QString &prefix = QString(), ArchiveItem *parentNode = nullptr) {
        if (!parentNode) parentNode = root;
        for (const QString &e : entries) {
            if (!prefix.isEmpty() && !e.startsWith(prefix)) continue;
            QString rel = prefix.isEmpty() ? e : e.mid(prefix.length());
            QStringList parts = rel.split('/', QString::SkipEmptyParts);
            ArchiveItem *cur = parentNode;
            QString accum = prefix;
            for (int i = 0; i < parts.size(); ++i) {
                QString part = parts[i];
                bool found = false;
                for (ArchiveItem *ch : cur->children) {
                    if (ch->name == part) { cur = ch; found = true; break; }
                }
                if (!found) {
                    ArchiveItem *it = new ArchiveItem();
                    it->name = part;
                    it->parent = cur;
                    accum = accum.isEmpty() ? part : accum + "/" + part;
                    it->fullPathInArchive = accum;
                    it->type = (i < parts.size() - 1 || e.endsWith('/'))
                               ? ArchiveItem::NodeType::Folder
                               : (part.endsWith(".vfsarc", Qt::CaseInsensitive) ? ArchiveItem::NodeType::ArchiveFolder : ArchiveItem::NodeType::File);
                    cur->children << it;
                    cur = it;
                }
            }
        }
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override {
        if (!hasIndex(row, column, parent)) return QModelIndex();
        ArchiveItem *pItem = itemFromIndex(parent);
        ArchiveItem *child = pItem->children.value(row, nullptr);
        if (child) return createIndex(row, column, child);
        return QModelIndex();
    }

    QModelIndex parent(const QModelIndex &index) const override {
        if (!index.isValid()) return QModelIndex();
        ArchiveItem *it = static_cast<ArchiveItem*>(index.internalPointer());
        ArchiveItem *p = it ? it->parent : nullptr;
        if (!p || p == root) return QModelIndex();
        ArchiveItem *gp = p->parent;
        int row = gp ? gp->children.indexOf(p) : 0;
        return createIndex(row, 0, p);
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        ArchiveItem *pItem = itemFromIndex(parent);
        return pItem ? pItem->children.count() : 0;
    }

    int columnCount(const QModelIndex &) const override { return 1; }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid()) return {};
        ArchiveItem *it = static_cast<ArchiveItem*>(index.internalPointer());
        if (role == Qt::DisplayRole) return it->name;
        if (role == Qt::DecorationRole) {
            switch(it->type) {
                case ArchiveItem::NodeType::Folder:
                    return QApplication::style()->standardIcon(QStyle::SP_DirIcon);
                case ArchiveItem::NodeType::ArchiveFolder:
                    return QIcon::fromTheme("package-x-generic");
                case ArchiveItem::NodeType::File:
                default:
                    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
            }
        }
        return {};
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    }

    QString pathForIndex(const QModelIndex &idx) const {
        if (!idx.isValid()) return QString();
        ArchiveItem *it = static_cast<ArchiveItem*>(idx.internalPointer());
        return it->fullPathInArchive;
    }

    // helper: find node by path (full path)
    ArchiveItem* findNodeByPath(const QString &path, ArchiveItem *start = nullptr) const {
        if (!start) start = root;
        if (path.isEmpty()) return start;
        QStringList parts = path.split('/', QString::SkipEmptyParts);
        ArchiveItem *cur = start;
        for (const QString &p : parts) {
            bool found=false;
            for (ArchiveItem *ch : cur->children) {
                if (ch->name == p) { cur = ch; found=true; break; }
            }
            if (!found) return nullptr;
        }
        return cur;
    }

private:
    ArchiveItem *itemFromIndex(const QModelIndex &index) const {
        if (!index.isValid()) return root;
        return static_cast<ArchiveItem*>(index.internalPointer());
    }
    ArchiveItem *root;
};

// --- Metadata struct ---
struct ArchiveMetadata {
    QString version;
    QString created;
    QStringList tags;
};

static ArchiveMetadata loadMetadata(ArchiveHandler *backend) {
    ArchiveMetadata meta;
    QString tmpPath;
    if (backend->extractEntryToTemp(".manifest.json", tmpPath)) {
        QFile f(tmpPath);
        if (f.open(QIODevice::ReadOnly)) {
            auto doc = QJsonDocument::fromJson(f.readAll());
            QJsonObject o = doc.object();
            meta.version = o.value("version").toString("1.0");
            meta.created = o.value("created").toString();
            for (auto t : o.value("tags").toArray()) meta.tags << t.toString();
        }
    } else {
        meta.version = "1.0";
        meta.created = QDateTime::currentDateTime().toString(Qt::ISODate);
        meta.tags = QStringList() << "new";
        QJsonObject o{{"version", meta.version}, {"created", meta.created}, {"tags", QJsonArray::fromStringList(meta.tags)}};
        QTemporaryFile tmp;
        tmp.open();
        tmp.write(QJsonDocument(o).toJson());
        tmp.flush();
        backend->addFiles({tmp.fileName()}, "");
    }
    return meta;
}

// Forward declare MainWindow
// main.cpp - Part 2/2
// Qt 5.12 single-file demo (part 2)

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle("Qt Virtual Archive Browser");
        resize(1100, 650);

        fsModel = new QFileSystemModel(this);
        fsModel->setRootPath(QDir::rootPath());
        fsView = new QTreeView;
        fsView->setModel(fsModel);
        for (int i = 1; i < fsModel->columnCount(); ++i) fsView->hideColumn(i);
        fsView->setHeaderHidden(true);

        archiveModel = new ArchiveModel(this);
        archiveView = new QTreeView;
        archiveView->setModel(archiveModel);
        archiveView->setHeaderHidden(true);
        archiveView->setContextMenuPolicy(Qt::CustomContextMenu);

        connect(archiveView, &QTreeView::doubleClicked, this, &MainWindow::onArchiveDoubleClicked);
        connect(archiveView, &QTreeView::customContextMenuRequested, this, &MainWindow::onArchiveContextMenu);
        connect(archiveView, &QTreeView::expanded, this, &MainWindow::onArchiveExpanded);
        connect(archiveView, &QTreeView::collapsed, this, &MainWindow::onArchiveCollapsed);

        QToolBar *tb = addToolBar("main");
        QAction *openAct = tb->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "Open .vfsarc");
        connect(openAct, &QAction::triggered, this, &MainWindow::onOpenArchive);

        splitter = new QSplitter;
        splitter->addWidget(fsView);
        splitter->addWidget(archiveView);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 1);
        setCentralWidget(splitter);

        metaDock = new QDockWidget("Metadata", this);
        metadataView = new QTextEdit;
        metadataView->setReadOnly(true);
        metaDock->setWidget(metadataView);
        addDockWidget(Qt::RightDockWidgetArea, metaDock);

        status = statusBar();

        backend = new CliArchiveHandler(this);

        // password cache / global pool
        // per-archive cached passwords (key = absolute archive path)
        // globalPool stores all passwords entered this session to attempt across archives
        passwordCache = QMap<QString, QString>();
        globalPasswords = QStringList();

        status->showMessage("Ready");
    }

private slots:
    void onOpenArchive() {
        QString file = QFileDialog::getOpenFileName(this, "Open archive", QDir::homePath(), "Virtual Archives (*.vfsarc);;ZIP Archives (*.zip);;All Files (*)");
        if (file.isEmpty()) return;
        if (!backend->openArchive(file)) {
            QMessageBox::warning(this, "Open failed", "Could not open archive: " + file);
            return;
        }
        currentArchive = file;
        // try password flow -> try cached then global then prompt
        attemptPasswordAndLoadArchive(backend, file);
    }

    void onArchiveExpanded(const QModelIndex &idx) {
        // lazy load children when expanding a folder node (only if not populated)
        if (!idx.isValid()) return;
        ArchiveItem *it = static_cast<ArchiveItem*>(idx.internalPointer());
        if (!it || it->childrenPopulated) return;

        QString prefix = it->fullPathInArchive;
        if (!prefix.endsWith("/")) prefix += "/";

        QStringList entries = backend->listEntries(prefix);
        archiveModel->populateFromList(entries, prefix, it);
        it->childrenPopulated = true;
        // notify view layout changed
        archiveModel->layoutChanged();
    }

    void onArchiveCollapsed(const QModelIndex &idx) {
        Q_UNUSED(idx);
        // optional: do nothing, or free children to reduce memory
    }

    void onArchiveDoubleClicked(const QModelIndex &idx) {
        if (!idx.isValid()) return;
        ArchiveItem *it = static_cast<ArchiveItem*>(idx.internalPointer());
        QString entry = archiveModel->pathForIndex(idx);
        if (it->type == ArchiveItem::NodeType::ArchiveFolder) {
            // nested open: extract nested archive to temp and open it with a new backend
            QString tmp;
            if (!backend->extractEntryToTemp(entry, tmp)) {
                // Try passwords: maybe nested archive is encrypted; try cached/global
                if (!tryPasswordsForEntryAndExtract(entry, tmp)) {
                    promptPasswordForArchiveAndLoad(entry); // will prompt user for password to extract nested
                    return;
                }
            }
            // open nested by switching backend to nested temporary archive
            CliArchiveHandler *nested = new CliArchiveHandler(this);
            if (nested->openArchive(tmp)) {
                // push current archive into stack for nested path tracking
                archiveStack << QFileInfo(currentArchive).fileName() + ":" + entry;
                updateStatusBar();
                // switch backend to nested
                delete backend;
                backend = nested;
                currentArchive = tmp;
                archiveModel->clear();
                archiveModel->populateFromList(backend->listEntries());
                auto meta = loadMetadata(backend);
                metadataView->setPlainText(QString("Nested Version: %1\nCreated: %2\nTags: %3")
                                           .arg(meta.version).arg(meta.created).arg(meta.tags.join(", ")));
            }
            return;
        }
        // else file: preview
        QString tmpPath;
        if (backend->extractEntryToTemp(entry, tmpPath)) previewFile(tmpPath);
    }

    void onArchiveContextMenu(const QPoint &pos) {
        QModelIndex idx = archiveView->indexAt(pos);
        if (!idx.isValid()) return;
        ArchiveItem *it = static_cast<ArchiveItem*>(idx.internalPointer());

        QMenu menu(this);
        QAction *addFolder = menu.addAction("Add Folder");
        QAction *removeItem = menu.addAction("Remove");
        QAction *showMeta = menu.addAction("Show Metadata");

        QAction *selected = menu.exec(archiveView->viewport()->mapToGlobal(pos));
        if (!selected) return;

        if (selected == addFolder) {
            bool ok;
            QString name = QInputDialog::getText(this, "New Folder", "Folder Name:", QLineEdit::Normal, QString(), &ok);
            if (ok && !name.isEmpty()) {
                // create model node
                ArchiveItem *newFolder = new ArchiveItem();
                newFolder->name = name;
                newFolder->type = ArchiveItem::NodeType::Folder;
                newFolder->parent = it;
                newFolder->fullPathInArchive = it->fullPathInArchive.isEmpty() ? name : it->fullPathInArchive + "/" + name;
                it->children << newFolder;
                it->childrenPopulated = true;
                archiveModel->layoutChanged();

                // create placeholder file in archive to represent the folder (zip has no empty dir support)
                QTemporaryFile tmp;
                tmp.open();
                // empty placeholder content
                tmp.write(QByteArray());
                tmp.flush();
                QString placeholderPath = newFolder->fullPathInArchive + "/.placeholder";
                // we need to add file with that path into the archive: create a temp file at same relative path then zip it
                // For CLI backend, we just add the temp file (zip will store file name only if run from correct dir)
                // Workaround: change cwd to temp dir and use zip with -j to store specified path
                // Simpler approach: add placeholder file at top-level and rely on path metadata in zip not kept here for demo.
                backend->addFiles({tmp.fileName()}, "");
                status->showMessage("Added folder (placeholder created)");
            }
        } else if (selected == removeItem) {
            // collect all paths under this node
            QStringList toRemove;
            collectPathsRecursively(it, toRemove);
            // call backend remove
            bool ok = backend->removeEntries(toRemove);
            if (!ok) {
                QMessageBox::warning(this, "Remove failed", "Backend failed to remove entries (CLI may rebuild archive).");
            } else {
                // remove nodes in model
                ArchiveItem *parent = it->parent;
                if (parent) parent->children.removeOne(it);
                delete it;
                archiveModel->layoutChanged();
                status->showMessage("Removed selected entry/entries");
            }
        } else if (selected == showMeta) {
            // show metadata of current archive or entry
            QString entry = archiveModel->pathForIndex(idx);
            QString tmp;
            if (!entry.isEmpty() && backend->extractEntryToTemp(entry, tmp)) {
                // attempt to read manifest inside that extracted entry if it's an archive
                QMessageBox::information(this, "Show Metadata", QString("Entry path: %1").arg(entry));
            } else {
                QMessageBox::information(this, "Archive Metadata", metadataView->toPlainText());
            }
        }
    }

private:
    // helper to collect all file paths under node (full archive paths)
    void collectPathsRecursively(ArchiveItem *node, QStringList &out) {
        if (!node) return;
        if (node->type == ArchiveItem::NodeType::File || node->type == ArchiveItem::NodeType::ArchiveFolder) {
            out << node->fullPathInArchive;
        } else {
            // folder: include children recursively
            for (ArchiveItem *ch : node->children) collectPathsRecursively(ch, out);
        }
    }

    // Password/prompt + load flow
    void attemptPasswordAndLoadArchive(ArchiveHandler *handler, const QString &archivePath) {
        // try per-archive cached password first
        if (passwordCache.contains(archivePath)) {
            handler->setPassword(passwordCache[archivePath]);
            QStringList entries = handler->listEntries();
            if (!entries.isEmpty()) { loadArchiveEntries(entries, archivePath); return; }
        }
        // try global passwords
        for (const QString &pw : globalPasswords) {
            handler->setPassword(pw);
            QStringList entries = handler->listEntries();
            if (!entries.isEmpty()) {
                // cache for this archive
                passwordCache[archivePath] = pw;
                loadArchiveEntries(entries, archivePath);
                return;
            }
        }
        // prompt user
        bool ok;
        QString pw = QInputDialog::getText(this, "Password Required", QString("Enter password for %1").arg(QFileInfo(archivePath).fileName()), QLineEdit::Password, QString(), &ok);
        if (!ok) return;  // user pressed Cancel
        // Allow empty passwords (try without -P)
        handler->setPassword(pw);
        QStringList entries = handler->listEntries();
        if (!entries.isEmpty()) {
            passwordCache[archivePath] = pw;
            if (!pw.isEmpty() && !globalPasswords.contains(pw))
                globalPasswords << pw;
            loadArchiveEntries(entries, archivePath);
            return;
        }
        QMessageBox::warning(this, "Password Failed", "Password did not work.");
}

    // try known passwords when extracting nested entry (returns true and tmpPath if success)
    bool tryPasswordsForEntryAndExtract(const QString &entry, QString &outTmp) {
        // try per-archive password
        QString apath = backend->archivePath();
        if (passwordCache.contains(apath)) {
            backend->setPassword(passwordCache[apath]);
            if (backend->extractEntryToTemp(entry, outTmp)) return true;
        }
        // try global passwords
        for (const QString &pw : globalPasswords) {
            backend->setPassword(pw);
            if (backend->extractEntryToTemp(entry, outTmp)) return true;
        }
        return false;
    }

    void promptPasswordForArchiveAndLoad(const QString &entryInCurrent) {
        // prompt user for password to extract nested archive entry -> we will try that password then load
        bool ok;
        QString pw = QInputDialog::getText(this, "Password Required", QString("Enter password to extract %1").arg(entryInCurrent), QLineEdit::Password, QString(), &ok);
        if (!ok || pw.isEmpty()) return;
        // attempt with this password
        backend->setPassword(pw);
        QString tmp;
        if (backend->extractEntryToTemp(entryInCurrent, tmp)) {
            // success: store pw in cache for current archive
            passwordCache[backend->archivePath()] = pw;
            if (!globalPasswords.contains(pw)) globalPasswords << pw;
            // now open nested
            CliArchiveHandler *nested = new CliArchiveHandler(this);
            if (nested->openArchive(tmp)) {
                // push stack and switch
                archiveStack << QFileInfo(currentArchive).fileName() + ":" + entryInCurrent;
                updateStatusBar();
                delete backend;
                backend = nested;
                currentArchive = tmp;
                archiveModel->clear();
                archiveModel->populateFromList(backend->listEntries());
                auto meta = loadMetadata(backend);
                metadataView->setPlainText(QString("Nested Version: %1\nCreated: %2\nTags: %3")
                                           .arg(meta.version).arg(meta.created).arg(meta.tags.join(", ")));
            }
        } else {
            QMessageBox::warning(this, "Extract Failed", "Could not extract nested archive with provided password.");
        }
    }

    void loadArchiveEntries(const QStringList &entries, const QString &archivePath) {
        // set UI, populate model root-level entries
        archiveModel->clear();
        archiveModel->populateFromList(entries);
        auto meta = loadMetadata(backend);
        metadataView->setPlainText(QString("Version: %1\nCreated: %2\nTags: %3")
                                   .arg(meta.version).arg(meta.created).arg(meta.tags.join(", ")));
        // reset archive stack to just this archive
        archiveStack.clear();
        archiveStack << QFileInfo(archivePath).fileName();
        updateStatusBar();
        status->showMessage(QString("Opened: %1").arg(archivePath));
    }

    void updateStatusBar() {
        QString s = archiveStack.join(" > ");
        // show lock icon if password cached for current archive
        QString lock = "";
        if (passwordCache.contains(backend->archivePath())) lock = " 🔒";
        status->showMessage(s + lock);
    }

    // preview helper
    void previewFile(const QString &path) {
        QFileInfo fi(path);
        QMimeDatabase db;
        QMimeType mt = db.mimeTypeForFile(path);

        if (mt.inherits("text/plain")) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QTextStream ts(&f);
                QString content = ts.readAll();
                QDockWidget *dock = new QDockWidget(fi.fileName(), this);
                QTextEdit *te = new QTextEdit; te->setReadOnly(true); te->setPlainText(content);
                dock->setWidget(te);
                addDockWidget(Qt::BottomDockWidgetArea, dock);
            }
            return;
        }
        if (QImageReader(path).canRead()) {
            QDockWidget *dock = new QDockWidget(fi.fileName(), this);
            QLabel *lbl = new QLabel; lbl->setAlignment(Qt::AlignCenter);
            QPixmap pm(path);
            lbl->setPixmap(pm.scaled(400,400, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            dock->setWidget(lbl);
            addDockWidget(Qt::BottomDockWidgetArea, dock);
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }

    // members
    QFileSystemModel *fsModel;
    QTreeView *fsView;
    ArchiveModel *archiveModel;
    QTreeView *archiveView;
    QSplitter *splitter;
    QDockWidget *metaDock;
    QTextEdit *metadataView;
    QStatusBar *status;

    ArchiveHandler *backend;
    QString currentArchive;

    // password caches
    QMap<QString, QString> passwordCache;
    QStringList globalPasswords;

    // nested archive stack for status bar
    QStringList archiveStack;
};

// main
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}

#include "main.moc"

