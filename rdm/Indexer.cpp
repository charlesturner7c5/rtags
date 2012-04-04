#include "Database.h"
#include "Indexer.h"
#include "Path.h"
#include "RTags.h"
#include "Rdm.h"
#include "SHA256.h"
#include "LevelDB.h"
#include "leveldb/write_batch.h"
#include <Log.h>
#include <QtCore>

// #define RDM_TIMING
#define SYNCINTERVAL 10

class IndexerJob;

typedef QHash<RTags::Location, Rdm::CursorInfo> SymbolHash;
typedef QHash<QByteArray, QSet<RTags::Location> > SymbolNameHash;
typedef QHash<Path, QSet<Path> > DependencyHash;
typedef QPair<QByteArray, quint64> WatchedPair;
typedef QHash<Path, QSet<WatchedPair> > WatchedHash;
typedef QHash<Path, QList<QByteArray> > InformationHash;

class DependencyEvent : public QEvent
{
public:
    enum { Type = QEvent::User + 1 };

    DependencyEvent(const DependencyHash& d)
        : QEvent(static_cast<QEvent::Type>(Type)), deps(d)
    {
    }

    DependencyHash deps;
};

class IndexerSyncer : public QThread
{
public:
    IndexerSyncer(QObject* parent = 0);

    void addSymbols(const SymbolHash &data);
    void addSymbolNames(const SymbolNameHash &symbolNames);
    void addDependencies(const DependencyHash& dependencies);
    void addFileInformation(const Path& input, const QList<QByteArray>& args);
    void notify();
    void stop();

protected:
    void run();

private:
    bool mStopped;
    QMutex mMutex;
    QWaitCondition mCond;
    SymbolHash mSymbols;
    SymbolNameHash mSymbolNames;
    DependencyHash mDependencies;
    InformationHash mInformations;
};

IndexerSyncer::IndexerSyncer(QObject* parent)
    : QThread(parent), mStopped(false)
{
}

void IndexerSyncer::stop()
{
    QMutexLocker locker(&mMutex);
    mStopped = true;
    mCond.wakeOne();
}

void IndexerSyncer::notify()
{
    QMutexLocker locker(&mMutex); // is this needed here?
    mCond.wakeOne();
}

void IndexerSyncer::addSymbolNames(const SymbolNameHash &locations)
{
    QMutexLocker lock(&mMutex);
    if (mSymbolNames.isEmpty()) {
        mSymbolNames = locations;
    } else {
        const SymbolNameHash::const_iterator end = locations.end();
        for (SymbolNameHash::const_iterator it = locations.begin(); it != end; ++it) {
            mSymbolNames[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::addSymbols(const SymbolHash &symbols)
{
    QMutexLocker lock(&mMutex);
    if (mSymbols.isEmpty()) {
        mSymbols = symbols;
    } else {
        const SymbolHash::const_iterator end = symbols.end();
        for (SymbolHash::const_iterator it = symbols.begin(); it != end; ++it) {
            mSymbols[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::addDependencies(const DependencyHash& dependencies)
{
    QMutexLocker lock(&mMutex);
    if (mDependencies.isEmpty()) {
        mDependencies = dependencies;
    } else {
        const DependencyHash::const_iterator end = dependencies.end();
        for (DependencyHash::const_iterator it = dependencies.begin(); it != end; ++it) {
            mDependencies[it.key()].unite(it.value());
        }
    }
}

void IndexerSyncer::addFileInformation(const Path& input, const QList<QByteArray>& args)
{
    QMutexLocker lock(&mMutex);
    mInformations[input] = args;
}

void IndexerSyncer::run()
{
    while (true) {
        SymbolNameHash symbolNames;
        SymbolHash symbols;
        DependencyHash dependencies;
        InformationHash informations;
        {
            QMutexLocker locker(&mMutex);
            if (mStopped)
                return;
            while (mSymbols.isEmpty() && mSymbolNames.isEmpty()) {
                mCond.wait(&mMutex, 10000);
                if (mStopped)
                    return;

            }
            qSwap(symbolNames, mSymbolNames);
            qSwap(symbols, mSymbols);
            qSwap(dependencies, mDependencies);
            qSwap(informations, mInformations);
        }
        if (!symbolNames.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::SymbolName, LevelDB::ReadWrite))
                return;

            leveldb::WriteBatch batch;

            SymbolNameHash::iterator it = symbolNames.begin();
            const SymbolNameHash::const_iterator end = symbolNames.end();
            bool changed = false;
            while (it != end) {
                const char *key = it.key().constData();
                const QSet<RTags::Location> added = it.value();
                QSet<RTags::Location> current = Rdm::readValue<QSet<RTags::Location> >(db.db(), key);
                const int oldSize = current.size();
                current += added;
                if (current.size() != oldSize) {
                    changed = true;
                    Rdm::writeValue<QSet<RTags::Location> >(&batch, key, current);
                }
                ++it;
            }

            if (changed)
                db.db()->Write(leveldb::WriteOptions(), &batch);
        }
        if (!symbols.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::Symbol, LevelDB::ReadWrite))
                return;

            leveldb::WriteBatch batch;

            SymbolHash::iterator it = symbols.begin();
            const SymbolHash::const_iterator end = symbols.end();
            bool changed = false;
            while (it != end) {
                const QByteArray key = it.key().key(RTags::Location::Padded);
                Rdm::CursorInfo added = it.value();
                Rdm::CursorInfo current = Rdm::readValue<Rdm::CursorInfo>(db.db(), key.constData());
                if (current.unite(added)) {
                    changed = true;
                    Rdm::writeValue<Rdm::CursorInfo>(&batch, key, current);
                }
                ++it;
            }

            if (changed)
                db.db()->Write(leveldb::WriteOptions(), &batch);
        }
        if (!dependencies.isEmpty()) {
            LevelDB db;
            if (!db.open(Database::Dependency, LevelDB::ReadWrite))
                return;

            leveldb::WriteBatch batch;

            DependencyHash::iterator it = dependencies.begin();
            const DependencyHash::const_iterator end = dependencies.end();
            bool changed = false;
            while (it != end) {
                const char* key = it.key().constData();
                QSet<Path> added = it.value();
                QSet<Path> current = Rdm::readValue<QSet<Path> >(db.db(), key);
                const int oldSize = current.size();
                if (current.unite(added).size() > oldSize) { // ### is this the correct way of checking if the current set has changed?
                    changed = true;
                    Rdm::writeValue<QSet<Path> >(&batch, key, current);
                }
                ++it;
            }

            if (changed)
                db.db()->Write(leveldb::WriteOptions(), &batch);
        }
        if (!informations.isEmpty()) {
            leveldb::WriteBatch batch;

            InformationHash::iterator it = informations.begin();
            const InformationHash::const_iterator end = informations.end();
            while (it != end) {
                const char *key = it.key().constData();
                Rdm::writeValue<QList<QByteArray> >(&batch, key, it.value());
                ++it;
            }
            LevelDB db;
            if (!db.open(Database::FileInformation, LevelDB::ReadWrite))
                return;

            db.db()->Write(leveldb::WriteOptions(), &batch);
        }
    }
}

class IndexerImpl
{
public:
    int jobCounter;

    Indexer* indexer;

    QMutex implMutex;
    QWaitCondition implCond;
    QSet<QByteArray> indexing;
    QSet<QByteArray> pchHeaderError;

    QByteArray path;
    int lastJobId;
    QHash<int, IndexerJob*> jobs;

    IndexerSyncer* syncer;

    bool timerRunning;
    QElapsedTimer timer;

    QList<QByteArray> defaultArgs;

    QFileSystemWatcher watcher;
    DependencyHash dependencies, pchDeps;
    mutable QReadWriteLock pchDependenciesLock;
    QMutex watchedMutex;
    WatchedHash watched;

    void setPchDependencies(const Path &pchHeader, const QSet<Path> &deps);
    QSet<Path> pchDependencies(const Path &pchHeader) const;
    void commitDependencies(const DependencyHash& deps);
};

inline void IndexerImpl::commitDependencies(const DependencyHash& deps)
{
    DependencyHash newDependencies;

    if (dependencies.isEmpty()) {
        dependencies = deps;
        newDependencies = deps;
    } else {
        const DependencyHash::const_iterator end = deps.end();
        for (DependencyHash::const_iterator it = deps.begin(); it != end; ++it) {
            newDependencies[it.key()].unite(it.value() - dependencies[it.key()]);
            dependencies[it.key()].unite(it.value());
        }
    }

    syncer->addDependencies(newDependencies);

    Path parentPath;
    QSet<QString> watchPaths;
    const DependencyHash::const_iterator end = newDependencies.end();
    QMutexLocker lock(&watchedMutex);
    for (DependencyHash::const_iterator it = newDependencies.begin(); it != end; ++it) {
        const Path& path = it.key();
        parentPath = path.parentDir();
        WatchedHash::iterator it = watched.find(parentPath);
        //debug() << "watching" << path << "in" << parentPath;
        if (it == watched.end()) {
            watched[parentPath].insert(qMakePair<QByteArray, quint64>(path.fileName(), path.lastModified()));
            watchPaths.insert(QString::fromLocal8Bit(parentPath));
        } else {
            it.value().insert(qMakePair<QByteArray, quint64>(path.fileName(), path.lastModified()));
        }
    }
    if (watchPaths.isEmpty())
        return;
    watcher.addPaths(watchPaths.toList());
}

void IndexerImpl::setPchDependencies(const Path &pchHeader, const QSet<Path> &deps)
{
    QWriteLocker lock(&pchDependenciesLock);
    if (deps.isEmpty()) {
        pchDeps.remove(pchHeader);
    } else {
        pchDeps[pchHeader] = deps;
    }
}

QSet<Path> IndexerImpl::pchDependencies(const Path &pchHeader) const
{
    QReadLocker lock(&pchDependenciesLock);
    return pchDeps.value(pchHeader);
}


struct Timestamp
{
    Timestamp()
        : count(0), ms(0)
    {}
    inline void add(int t)
    {
        ms += t;
        ++count;
    }
    int count, ms;
};

class IndexerJob : public QObject, public QRunnable
{
    Q_OBJECT
    public:
    IndexerJob(IndexerImpl* impl, int id,
               const Path& path, const Path& input,
               const QList<QByteArray>& arguments);

    int id() const { return mId; }

    void run();

    int mId;
    bool mIsPch;
    RTags::Location createLocation(CXCursor cursor);
    void addNamePermutations(CXCursor cursor, const RTags::Location &location);

    SymbolHash mSymbols;
    SymbolNameHash mSymbolNames;

    QSet<Path> mPaths;
    QHash<RTags::Location, QPair<RTags::Location, bool> > mReferences;
    Path mPath, mIn;
    QList<QByteArray> mArgs;
    DependencyHash mDependencies;
    QSet<Path> mPchDependencies;
    IndexerImpl* mImpl;
#ifdef RDM_TIMING
    QHash<int, Timestamp> mTimeStamps;
#endif
signals:
    void done(int id, const QByteArray& input);
};

class DirtyJob : public QObject, public QRunnable
{
public:
    DirtyJob(const QSet<Path> &dirty,
             const QHash<Path, QList<QByteArray> > &toIndexPch,
             const QHash<Path, QList<QByteArray> > &toIndex)
        : mDirty(dirty), mToIndexPch(toIndexPch), mToIndex(toIndex)
    {}

    virtual void run()
    {
        dirty();
        Indexer *indexer = Indexer::instance();
        for (QHash<Path, QList<QByteArray> >::const_iterator it = mToIndexPch.begin(); it != mToIndexPch.end(); ++it)
            indexer->index(it.key(), it.value());
        for (QHash<Path, QList<QByteArray> >::const_iterator it = mToIndex.begin(); it != mToIndex.end(); ++it)
            indexer->index(it.key(), it.value());
    }
    void dirty();
private:
    const QSet<Path> mDirty;
    const QHash<Path, QList<QByteArray> > mToIndexPch, mToIndex;
};

#include "Indexer.moc"

static void inclusionVisitor(CXFile included_file,
                             CXSourceLocation* include_stack,
                             unsigned include_len,
                             CXClientData client_data)
{
    (void)include_len;
    (void)included_file;
    IndexerJob* job = static_cast<IndexerJob*>(client_data);
    CXString fn = clang_getFileName(included_file);
    const char *cstr = clang_getCString(fn);
    // ### make this configurable
    if ((strncmp("/usr/", cstr, 5) != 0)
        || (strncmp("/usr/home/", cstr, 10) == 0)) {
        Path path = Path::canonicalized(cstr);
        foreach (const QByteArray& arg, job->mImpl->defaultArgs) {
            if (arg.contains(path)) {
                clang_disposeString(fn);
                return;
            }
        }
        for (unsigned i=0; i<include_len; ++i) {
            CXFile originatingFile;
            clang_getSpellingLocation(include_stack[i], &originatingFile, 0, 0, 0);
            CXString originatingFn = clang_getFileName(originatingFile);
            job->mDependencies[path].insert(Path::canonicalized(clang_getCString(originatingFn)));
            clang_disposeString(originatingFn);
        }
        if (!include_len) {
            job->mDependencies[path].insert(path);
        }
        if (job->mIsPch) {
            job->mPchDependencies.insert(path);
        }
    }
    clang_disposeString(fn);
}

void IndexerJob::addNamePermutations(CXCursor cursor, const RTags::Location &location)
{
    QByteArray qname;
    QByteArray qparam, qnoparam;

    CXString displayName;
    CXCursor cur = cursor, null = clang_getNullCursor();
    CXCursorKind kind;
    for (;;) {
        if (clang_equalCursors(cur, null))
            break;
        kind = clang_getCursorKind(cur);
        if (clang_isTranslationUnit(kind))
            break;

        displayName = clang_getCursorDisplayName(cur);
        const char* name = clang_getCString(displayName);
        if (!name || !strlen(name)) {
            clang_disposeString(displayName);
            break;
        }
        qname = QByteArray(name);
        if (qparam.isEmpty()) {
            qparam.prepend(qname);
            qnoparam.prepend(qname);
            const int sp = qnoparam.indexOf('(');
            if (sp != -1)
                qnoparam = qnoparam.left(sp);
        } else {
            qparam.prepend(qname + "::");
            qnoparam.prepend(qname + "::");
        }
        Q_ASSERT(!qparam.isEmpty());
        mSymbolNames[qparam].insert(location);
        if (qparam != qnoparam) {
            Q_ASSERT(!qnoparam.isEmpty());
            mSymbolNames[qnoparam].insert(location);
        }

        clang_disposeString(displayName);
        cur = clang_getCursorSemanticParent(cur);
    }
}

RTags::Location IndexerJob::createLocation(CXCursor cursor)
{
    CXSourceLocation location = clang_getCursorLocation(cursor);
    RTags::Location ret;
    if (!clang_equalLocations(location, clang_getNullLocation())) {
        CXFile file;
        unsigned start;
        clang_getSpellingLocation(location, &file, 0, 0, &start);
        CXString fn = clang_getFileName(file);
        const char *fileName = clang_getCString(fn);
        if (fileName && strlen(fileName)) {
            ret.path = fileName;
            ret.path.canonicalize(); // ### could canonicalize directly
            ret.offset = start;
            mPaths.insert(ret.path);
        }
        // unsigned l, c;
        // clang_getSpellingLocation(location, 0, &l, &c, 0);
        // QByteArray out;
        // out.append(ret.path);
        // out.append(':');
        // out.append(QByteArray::number(l));
        // out.append(':');
        // out.append(QByteArray::number(c));
        // debug() << ret.key() << "is" << out;
        clang_disposeString(fn);
    }
    return ret;
}

#ifdef RDM_TIMING
#define RDM_TIMESTAMP() job->mTimeStamps[__LINE__].add(timer.restart())
#define RDM_END_TIMESTAMP(file)                                         \
    printf("%s\n", file);                                               \
    for (QHash<int, Timestamp>::const_iterator it = mTimeStamps.begin(); it != mTimeStamps.end(); ++it) { \
        printf("    line: %d total: %dms count: %d average: %fms\n",    \
               it.key(), it.value().ms, it.value().count,               \
               it.value().count                                         \
               ? double(it.value().ms) / it.value().count               \
               : 0.0);                                                  \
    }                                                                   \

#else
#define RDM_TIMESTAMP()
#define RDM_END_TIMESTAMP(file)
#endif

static CXChildVisitResult indexVisitor(CXCursor cursor,
                                       CXCursor /*parent*/,
                                       CXClientData client_data)
{
#ifdef QT_DEBUG
    {
        CXCursor ref = clang_getCursorReferenced(cursor);
        if (clang_equalCursors(cursor, ref) && !clang_isCursorDefinition(ref)) {
            ref = clang_getCursorDefinition(ref);
        }
        debug() << Rdm::cursorToString(cursor) << "refs" << Rdm::cursorToString(clang_getCursorReferenced(cursor))
                << (clang_equalCursors(ref, clang_getCursorReferenced(cursor)) ? QByteArray() : ("changed to " + Rdm::cursorToString(ref)));
    }
#endif
#ifdef RDM_TIMING
    QElapsedTimer timer;
    timer.start();
#endif
    IndexerJob* job = static_cast<IndexerJob*>(client_data);

    const CXCursorKind kind = clang_getCursorKind(cursor);
    switch (kind) {
    case CXCursor_CXXAccessSpecifier:
        return CXChildVisit_Recurse;
    default:
        break;
    }

    const RTags::Location loc = job->createLocation(cursor);
    RDM_TIMESTAMP();
    if (loc.isNull()) {
        return CXChildVisit_Recurse;
    }
    CXCursor ref = clang_getCursorReferenced(cursor);
    if (clang_equalCursors(cursor, ref) && !clang_isCursorDefinition(ref)) {
        // QByteArray old = Rdm::cursorToString(ref);
        ref = clang_getCursorDefinition(ref);
        // error() << "changed ref from" << old << "to" << Rdm::cursorToString(ref);
    }
    const CXCursorKind refKind = clang_getCursorKind(ref);
    RDM_TIMESTAMP();

    Rdm::CursorInfo &info = job->mSymbols[loc];
    if (kind == CXCursor_CallExpr && refKind == CXCursor_CXXMethod) {
        return CXChildVisit_Recurse;
    } else if (!info.symbolLength) {
        info.kind = kind;
    } else if (info.kind == CXCursor_Constructor && kind == CXCursor_TypeRef) {
        return CXChildVisit_Recurse;
    }
    if (!info.symbolLength) {
        CXString name;
        if (clang_isReference(kind)) {
            name = clang_getCursorSpelling(ref);
        } else {
            name = clang_getCursorSpelling(cursor);
        }
        const char *cstr = clang_getCString(name);
        info.symbolLength = cstr ? strlen(cstr) : 0;
        clang_disposeString(name);
        RDM_TIMESTAMP();
    }

    if (clang_isCursorDefinition(cursor) || kind == CXCursor_FunctionDecl) {
        job->addNamePermutations(cursor, loc);
        RDM_TIMESTAMP();
    }


    if (!clang_isInvalid(refKind) && !clang_equalCursors(cursor, ref)) {
        const RTags::Location refLoc = job->createLocation(ref);
        RDM_TIMESTAMP();
        if (refLoc.isNull()) {
            return CXChildVisit_Recurse;
        }

        info.target = refLoc;
        bool isMemberFunction = false;
        // error() << "we're here" << Rdm::cursorToString(ref)
        //         << Rdm::cursorToString(cursor);
        if (refKind == kind) {
            switch (refKind) {
            case CXCursor_Constructor:
            case CXCursor_Destructor:
            case CXCursor_CXXMethod:
                isMemberFunction = true;
                // error() << "got shit called" << loc << "ref is" << refLoc
                //         << Rdm::cursorToString(cursor) << "is" << Rdm::cursorToString(ref);
                break;
            default:
                break;
            }
        }
        job->mReferences[loc] = qMakePair(refLoc, isMemberFunction);
        RDM_TIMESTAMP();
    }
    return CXChildVisit_Recurse;

}

IndexerJob::IndexerJob(IndexerImpl* impl, int id,
                       const Path& path, const Path& input,
                       const QList<QByteArray>& arguments)
    : mId(id), mIsPch(false), mPath(path), mIn(input), mArgs(arguments), mImpl(impl)
{
}

static inline QList<Path> extractPchFiles(const QList<QByteArray>& args)
{
    QList<Path> out;
    bool nextIsPch = false;
    foreach (const QByteArray& arg, args) {
        if (arg.isEmpty())
            continue;

        if (nextIsPch) {
            nextIsPch = false;
            out.append(arg);
        } else if (arg == "-include-pch") {
            nextIsPch = true;
        }
    }
    return out;
}

static QByteArray pchFileName(const QByteArray &path, const QByteArray &header)
{
    return path + SHA256::hash(header.constData());
}

void IndexerJob::run()
{
    QElapsedTimer timer;
    timer.start();
    QList<QByteArray> args = mArgs + mImpl->defaultArgs;
    QList<Path> pchHeaders = extractPchFiles(args);
    if (!pchHeaders.isEmpty()) {
        QMutexLocker locker(&mImpl->implMutex);
        bool wait;
        do {
            wait = false;
            foreach (const QByteArray &pchHeader, pchHeaders) {
                if (mImpl->pchHeaderError.contains(pchHeader)) {
                    int idx = args.indexOf(pchHeader);
                    Q_ASSERT(idx > 0);
                    args.removeAt(idx);
                    args.removeAt(idx - 1);
                } else if (mImpl->indexing.contains(pchHeader)) {
                    wait = true;
                    break;
                }
            }
            if (wait) {
                mImpl->implCond.wait(&mImpl->implMutex);
            }
        } while (wait);
    }
    const quint64 waitingForPch = timer.restart();

    QVarLengthArray<const char*, 32> clangArgs(args.size());
    QByteArray clangLine = "clang ";
    bool nextIsPch = false, nextIsX = false;
    QByteArray pchName;

    QList<Path> pchFiles;
    int idx = 0;
    foreach (const QByteArray& arg, args) {
        if (arg.isEmpty())
            continue;

        if (nextIsPch) {
            nextIsPch = false;
            pchFiles.append(pchFileName(mImpl->path, arg));
            clangArgs[idx++] = pchFiles.last().constData();
            clangLine += pchFiles.last().constData();
            clangLine += " ";
            continue;
        }

        if (nextIsX) {
            nextIsX = false;
            mIsPch = (arg == "c++-header" || arg == "c-header");
        }
        clangArgs[idx++] = arg.constData();
        clangLine += arg;
        clangLine += " ";
        if (arg == "-include-pch") {
            nextIsPch = true;
        } else if (arg == "-x") {
            nextIsX = true;
        }
    }
    if (mIsPch) {
        pchName = pchFileName(mImpl->path, mIn);
    }
    clangLine += mIn;

    CXIndex index = clang_createIndex(1, 1);
    CXTranslationUnit unit = clang_parseTranslationUnit(index, mIn.constData(),
                                                        clangArgs.data(), idx,
                                                        0, 0, CXTranslationUnit_Incomplete);
    log(1) << "loading unit" << clangLine << (unit != 0);
    bool pchError = false;

    if (!unit) {
        pchError = mIsPch;
        error() << "got 0 unit for" << clangLine;
    } else {
        clang_getInclusions(unit, inclusionVisitor, this);
        foreach(const Path &pchHeader, pchHeaders) {
            foreach(const Path &dep, mImpl->pchDependencies(pchHeader)) {
                mDependencies[dep].insert(mIn);
            }
        }
        QCoreApplication::postEvent(mImpl->indexer, new DependencyEvent(mDependencies));

        clang_visitChildren(clang_getTranslationUnitCursor(unit), indexVisitor, this);
        RDM_END_TIMESTAMP(mIn.constData());
        if (mIsPch) {
            Q_ASSERT(!pchName.isEmpty());
            if (clang_saveTranslationUnit(unit, pchName.constData(), clang_defaultSaveOptions(unit)) != CXSaveError_None) {
                error() << "Couldn't save pch file" << mIn << pchName;
                pchError = true;
            }
        }
        clang_disposeTranslationUnit(unit);

        const QHash<RTags::Location, QPair<RTags::Location, bool> >::const_iterator end = mReferences.end();
        for (QHash<RTags::Location, QPair<RTags::Location, bool> >::const_iterator it = mReferences.begin(); it != end; ++it) {
            SymbolHash::iterator sym = mSymbols.find(it.value().first);
            if (sym != mSymbols.end()) {
                // Q_ASSERT(mSymbols.contains(it.value().first));
                // debug() << "key" << it.key() << "value" << it.value();
                Rdm::CursorInfo &ci = sym.value();
                if (it.value().second) {
                    Rdm::CursorInfo &otherCi = mSymbols[it.key()];
                    // ### kinda nasty
                    ci.references += otherCi.references;
                    otherCi.references = ci.references;
                    if (otherCi.target.isNull())
                        ci.target = it.key();
                } else {
                    ci.references.insert(it.key());
                }
            }
        }

        {
            SymbolHash::iterator it = mSymbols.begin();
            const SymbolHash::const_iterator end = mSymbols.end();
            while (it != end) {
                Rdm::CursorInfo &ci = it.value();
                if (ci.target.isNull() && ci.references.isEmpty()) {
                    it = mSymbols.erase(it);
                } else {
                    debug() << it.key() << it.value().symbolLength << "=>" << it.value().target
                            << it.value().references;
                    ++it;
                }
            }
        }
        foreach (const Path &path, mPaths) {
            const RTags::Location loc(path, 1);
            mSymbolNames[path].insert(loc);
            mSymbolNames[path.fileName()].insert(loc);
        }
        mImpl->syncer->addSymbols(mSymbols);
        mImpl->syncer->addSymbolNames(mSymbolNames);
        mImpl->syncer->addFileInformation(mIn, mArgs);
        if (mIsPch)
            mImpl->setPchDependencies(mIn, mPchDependencies);

    }
    clang_disposeIndex(index);
    if (mIsPch) {
        QMutexLocker locker(&mImpl->implMutex);
        if (pchError) {
            mImpl->pchHeaderError.insert(mIn);
        } else {
            mImpl->pchHeaderError.remove(mIn);
        }
    }
    emit done(mId, mIn);
    log(0) << "visited" << mIn << timer.elapsed()
           << qPrintable(waitingForPch ? QString("Waited for pch: %1ms.").arg(waitingForPch) : QString());
}

Indexer* Indexer::sInst = 0;

Indexer::Indexer(const QByteArray& path, QObject* parent)
    : QObject(parent), mImpl(new IndexerImpl)
{
    Q_ASSERT(path.startsWith('/'));
    if (!path.startsWith('/'))
        return;
    QDir dir;
    dir.mkpath(path);

    mImpl->indexer = this;
    mImpl->jobCounter = 0;
    mImpl->lastJobId = 0;
    mImpl->path = path;
    if (!mImpl->path.endsWith('/'))
        mImpl->path += '/';
    mImpl->timerRunning = false;
    mImpl->syncer = new IndexerSyncer(this);
    mImpl->syncer->start();

    connect(&mImpl->watcher, SIGNAL(directoryChanged(QString)),
            this, SLOT(onDirectoryChanged(QString)));

    sInst = this;
}

Indexer::~Indexer()
{
    sInst = 0;
    mImpl->syncer->stop();
    mImpl->syncer->wait();

    delete mImpl;
}

Indexer* Indexer::instance()
{
    return sInst;
}

int Indexer::index(const QByteArray& input, const QList<QByteArray>& arguments)
{
    QMutexLocker locker(&mImpl->implMutex);

    if (mImpl->indexing.contains(input))
        return -1;

    int id;
    do {
        id = mImpl->lastJobId++;
    } while (mImpl->jobs.contains(id));

    mImpl->indexing.insert(input);

    IndexerJob* job = new IndexerJob(mImpl, id, mImpl->path, input, arguments);
    mImpl->jobs[id] = job;
    connect(job, SIGNAL(done(int, QByteArray)), this, SLOT(onJobDone(int, QByteArray)), Qt::QueuedConnection);

    if (!mImpl->timerRunning) {
        mImpl->timerRunning = true;
        mImpl->timer.start();
    }

    QThreadPool::globalInstance()->start(job);

    return id;
}

void Indexer::customEvent(QEvent* e)
{
    if (e->type() == static_cast<QEvent::Type>(DependencyEvent::Type)) {
        mImpl->commitDependencies(static_cast<DependencyEvent*>(e)->deps);
    }
}

static inline bool isPch(const QList<QByteArray> &args)
{
    const int size = args.size();
    bool nextIsX = false;
    for (int i=0; i<size; ++i) {
        const QByteArray &arg = args.at(i);
        if (nextIsX) {
            return (arg == "c++-header" || arg == "c-header");
        } else if (arg == "-x") { // ### this is not entirely safe, -xc++-header is allowed
            nextIsX = true;
        }
    }
    return false;
}

void Indexer::onDirectoryChanged(const QString& path)
{
    const Path p = path.toLocal8Bit();
    Q_ASSERT(p.endsWith('/'));
    QMutexLocker lock(&mImpl->watchedMutex);
    WatchedHash::iterator it = mImpl->watched.find(p);
    if (it == mImpl->watched.end()) {
        error() << "directory changed, but not in watched list" << p;
        return;
    }

    Path file;
    QList<Path> pending;
    QSet<WatchedPair>::iterator wit = it.value().begin();
    QSet<WatchedPair>::const_iterator wend = it.value().end();
    QList<QByteArray> args;
    QSet<Path> dirtyFiles;
    QHash<Path, QList<QByteArray> > toIndex, toIndexPch;

    LevelDB db;
    QByteArray err;
    if (!db.open(Database::FileInformation, LevelDB::ReadOnly, &err)) {
        // ### there is a gap here where if the syncer thread hasn't synced the file information
        //     then fileInformation() would return 'false' even though it knows what args to return.
        error("Can't open FileInformation database %s %s\n",
              Database::databaseName(Database::FileInformation).constData(),
              err.constData());
        return;
    }
    while (wit != wend) {
        // weird API, QSet<>::iterator does not allow for modifications to the referenced value
        file = (p + (*wit).first);
        if (!file.exists() || file.lastModified() != (*wit).second) {
            dirtyFiles.insert(file);
            pending.append(file);
            wit = it.value().erase(wit);
            wend = it.value().end(); // ### do we need to update 'end' here?

            DependencyHash::const_iterator dit = mImpl->dependencies.find(file);
            if (dit == mImpl->dependencies.end()) {
                error() << "file modified but not in dependency list" << file;
                ++it;
                continue;
            }
            Q_ASSERT(!dit.value().isEmpty());
            foreach (const Path& path, dit.value()) {
                dirtyFiles.insert(path);
                if (path.exists()) {
                    bool ok;
                    args = Rdm::readValue<QList<QByteArray> >(db.db(), path, &ok);

                    if (ok) {
                        if (isPch(args)) {
                            toIndexPch[path] = args;
                        } else {
                            toIndex[path] = args;
                        }
                    }
                }
            }
        } else {
            ++wit;
        }
    }

    foreach (const Path& path, pending) {
        it.value().insert(qMakePair<QByteArray, quint64>(path.fileName(), path.lastModified()));
    }
    lock.unlock();
    QThreadPool::globalInstance()->start(new DirtyJob(dirtyFiles, toIndexPch, toIndex));
}

void Indexer::onJobDone(int id, const QByteArray& input)
{
    Q_UNUSED(input)

        QMutexLocker locker(&mImpl->implMutex);
    mImpl->jobs.remove(id);
    if (mImpl->indexing.remove(input))
        mImpl->implCond.wakeAll();

    ++mImpl->jobCounter;

    if (mImpl->jobs.isEmpty() || mImpl->jobCounter == SYNCINTERVAL) {
        if (mImpl->jobs.isEmpty()) {
            mImpl->syncer->notify();

            Q_ASSERT(mImpl->timerRunning);
            mImpl->timerRunning = false;
            log(0) << "jobs took" << mImpl->timer.elapsed() << "ms";
        }
    }

    emit indexingDone(id);
}

void Indexer::setDefaultArgs(const QList<QByteArray> &args)
{
    mImpl->defaultArgs = args;
}

void DirtyJob::dirty()
{
    // ### we should probably have a thread or something that stats each file we have in the db and calls dirty if the file is gone
    const leveldb::WriteOptions writeOptions;
    debug() << "DirtyJob::dirty" << mDirty;
    {
        LevelDB db;
        QByteArray err;
        if (!db.open(Database::Symbol, LevelDB::ReadWrite, &err)) {
            error("Can't open symbol database %s %s\n",
                  Database::databaseName(Database::Symbol).constData(),
                  err.constData());
        }
        leveldb::Iterator* it = db.db()->NewIterator(leveldb::ReadOptions());
        leveldb::WriteBatch batch;
        bool writeBatch = false;
        it->SeekToFirst();
        while (it->Valid()) {
            const leveldb::Slice key = it->key();
            debug() << "looking at" << key.data();
            const int comma = QByteArray::fromRawData(key.data(), key.size()).lastIndexOf(',');
            Q_ASSERT(comma != -1);
            const Path p = QByteArray::fromRawData(key.data(), comma);
            if (mDirty.contains(p)) {
                debug() << "key is dirty. removing" << key.data();
                batch.Delete(key);
                writeBatch = true;
            } else {
                Rdm::CursorInfo cursorInfo = Rdm::readValue<Rdm::CursorInfo>(it);
                if (cursorInfo.dirty(mDirty)) {
                    writeBatch = true;
                    if (cursorInfo.target.isNull() && cursorInfo.references.isEmpty()) {
                        debug() << "CursorInfo is empty now. removing" << key.data();
                        batch.Delete(key);
                    } else {
                        debug() << "CursorInfo is modified. Changing" << key.data();
                        Rdm::writeValue<Rdm::CursorInfo>(&batch, key.data(), cursorInfo);
                    }
                }
            }
            it->Next();
        }
        delete it;
        if (writeBatch) {
            db.db()->Write(writeOptions, &batch);
        }
    }

    {
        LevelDB db;
        QByteArray err;
        if (!db.open(Database::SymbolName, LevelDB::ReadWrite, &err)) {
            error("Can't open symbol name database %s %s\n",
                  Database::databaseName(Database::SymbolName).constData(),
                  err.constData());
        }
        leveldb::Iterator* it = db.db()->NewIterator(leveldb::ReadOptions());
        leveldb::WriteBatch batch;
        bool writeBatch = false;
        it->SeekToFirst();
        while (it->Valid()) {
            QSet<RTags::Location> locations = Rdm::readValue<QSet<RTags::Location> >(it);
            QSet<RTags::Location>::iterator i = locations.begin();
            bool changed = false;
            while (i != locations.end()) {
                if (mDirty.contains((*i).path)) {
                    changed = true;
                    i = locations.erase(i);
                } else {
                    ++i;
                }
            }
            if (changed) {
                writeBatch = true;
                if (locations.isEmpty()) {
                    debug() << "No references to" << it->key().data() << "anymore. Removing";
                    batch.Delete(it->key());
                } else {
                    debug() << "References to" << it->key().data() << "modified. Changing";
                    Rdm::writeValue<QSet<RTags::Location> >(&batch, it->key().data(), locations);
                }
            }
            it->Next();
        }
        delete it;
        if (writeBatch) {
            db.db()->Write(writeOptions, &batch);
        }
    }
}
