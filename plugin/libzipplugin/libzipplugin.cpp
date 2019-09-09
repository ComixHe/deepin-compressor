/*
 * Copyright (c) 2017 Ragnar Thomsen <rthomsen6@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ( INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION ) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * ( INCLUDING NEGLIGENCE OR OTHERWISE ) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "libzipplugin.h"
//#include "kpluginfactory.h"


//#include <KIO/Global>
//#include <KLocalizedString>


#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <qplatformdefs.h>
#include <QThread>

#include <utime.h>
#include <zlib.h>
#include <memory>
#include "queries.h"

//K_PLUGIN_CLASS_WITH_JSON(LibzipPlugin, "kerfuffle_libzip.json")

LibzipPluginFactory::LibzipPluginFactory()
{
    registerPlugin<LibzipPlugin>();
}
LibzipPluginFactory::~LibzipPluginFactory()
{

}

void LibzipPlugin::progressCallback(zip_t *, double progress, void *that)
{
    static_cast<LibzipPlugin *>(that)->emitProgress(progress);
}

LibzipPlugin::LibzipPlugin(QObject *parent, const QVariantList & args)
    : ReadWriteArchiveInterface(parent, args)
    , m_overwriteAll(false)
    , m_skipAll(false)
    , m_listAfterAdd(false)
{
}

LibzipPlugin::~LibzipPlugin()
{
    for (const auto e : qAsConst(m_emittedEntries)) {
        // Entries might be passed to pending slots, so we just schedule their deletion.
        e->deleteLater();
    }
}

bool LibzipPlugin::list()
{
    m_numberOfEntries = 0;

    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), ZIP_RDONLY, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (!archive) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    // Fetch archive comment.
    m_comment = QString::fromLocal8Bit(zip_get_archive_comment(archive, nullptr, ZIP_FL_ENC_RAW));

    // Get number of archive entries.
    const auto nofEntries = zip_get_num_entries(archive, 0);

    // Loop through all archive entries.
    for (int i = 0; i < nofEntries; i++) {

        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }

        emitEntryForIndex(archive, i);
        emit progress(float(i + 1) / nofEntries);
    }

    zip_close(archive);
    m_listAfterAdd = false;
    return true;
}

bool LibzipPlugin::addFiles(const QVector<Archive::Entry*> &files, const Archive::Entry *destination, const CompressionOptions& options, uint numberOfEntriesToAdd)
{
    Q_UNUSED(numberOfEntriesToAdd)
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), ZIP_CREATE, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (!archive) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    uint i = 0;
    for (const Archive::Entry* e : files) {

        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }

        // If entry is a directory, traverse and add all its files and subfolders.
        if (QFileInfo(e->fullPath()).isDir()) {

            if (!writeEntry(archive, e->fullPath(), destination, options, true)) {
                return false;
            }

            QDirIterator it(e->fullPath(),
                            QDir::AllEntries | QDir::Readable |
                            QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);

            while (!QThread::currentThread()->isInterruptionRequested() && it.hasNext()) {
                const QString path = it.next();

                if (QFileInfo(path).isDir()) {
                    if (!writeEntry(archive, path, destination, options, true)) {
                        return false;
                    }
                } else {
                    if (!writeEntry(archive, path, destination, options)) {
                        return false;
                    }
                }
                i++;
            }
        } else {
            if (!writeEntry(archive, e->fullPath(), destination, options)) {
                return false;
            }
        }
        i++;

    }
    m_filesize = i;
    m_addarchive = archive;
    // Register the callback function to get progress feedback.
    zip_register_progress_callback_with_state(archive, 0.001, progressCallback, nullptr, this);

    if (zip_close(archive)) {
        emit error(tr("Failed to write archive."));
        return false;
    }

    // We list the entire archive after adding files to ensure entry
    // properties are up-to-date.
    m_listAfterAdd = true;
    list();

    return true;
}

void LibzipPlugin::emitProgress(double percentage)
{
    int i = m_filesize*percentage;
    if(m_addarchive)
    {
        QString name = zip_get_name(m_addarchive, i, ZIP_FL_ENC_GUESS);
        emit progress_filename(name);
    }

    // Go from 0 to 50%. The second half is the subsequent listing.
    emit progress(percentage);
}

bool LibzipPlugin::writeEntry(zip_t *archive, const QString &file, const Archive::Entry* destination, const CompressionOptions& options, bool isDir)
{
    Q_ASSERT(archive);

    QByteArray destFile;
    if (destination) {
        destFile = QString(destination->fullPath() + file).toUtf8();
    } else {
        destFile = file.toUtf8();
    }

    qlonglong index;
    if (isDir) {
        index = zip_dir_add(archive, destFile.constData(), ZIP_FL_ENC_GUESS);
        if (index == -1) {
            // If directory already exists in archive, we get an error.
            return true;
        }
    } else {
        zip_source_t *src = zip_source_file(archive, QFile::encodeName(file).constData(), 0, -1);
        Q_ASSERT(src);

        index = zip_file_add(archive, destFile.constData(), src, ZIP_FL_ENC_GUESS | ZIP_FL_OVERWRITE);
        if (index == -1) {
            zip_source_free(src);
            emit error(tr("Failed to add entry: %1"));
            return false;
        }
    }

#ifndef Q_OS_WIN
    // Set permissions.
    QT_STATBUF result;
    if (QT_STAT(QFile::encodeName(file).constData(), &result) != 0) {
    } else {
        zip_uint32_t attributes = result.st_mode << 16;
        if (zip_file_set_external_attributes(archive, index, ZIP_FL_UNCHANGED, ZIP_OPSYS_UNIX, attributes) != 0) {
        }
    }
#endif

    if (!password().isEmpty()) {
        Q_ASSERT(!options.encryptionMethod().isEmpty());
        if (options.encryptionMethod() == QLatin1String("AES128")) {
            zip_file_set_encryption(archive, index, ZIP_EM_AES_128, password().toUtf8().constData());
        } else if (options.encryptionMethod() == QLatin1String("AES192")) {
            zip_file_set_encryption(archive, index, ZIP_EM_AES_192, password().toUtf8().constData());
        } else if (options.encryptionMethod() == QLatin1String("AES256")) {
            zip_file_set_encryption(archive, index, ZIP_EM_AES_256, password().toUtf8().constData());
        }
    }

    // Set compression level and method.
    zip_int32_t compMethod = ZIP_CM_DEFAULT;
    if (!options.compressionMethod().isEmpty()) {
        if (options.compressionMethod() == QLatin1String("Deflate")) {
            compMethod = ZIP_CM_DEFLATE;
        } else if (options.compressionMethod() == QLatin1String("BZip2")) {
            compMethod = ZIP_CM_BZIP2;
        } else if (options.compressionMethod() == QLatin1String("Store")) {
            compMethod = ZIP_CM_STORE;
        }
    }
    const int compLevel = options.isCompressionLevelSet() ? options.compressionLevel() : 6;
    if (zip_set_file_compression(archive, index, compMethod, compLevel) != 0) {
        emit error(tr("Failed to set compression options for entry: %1"));
        return false;
    }

    return true;
}

bool LibzipPlugin::emitEntryForIndex(zip_t *archive, qlonglong index)
{
    Q_ASSERT(archive);

    zip_stat_t statBuffer;
    if (zip_stat_index(archive, index, ZIP_FL_ENC_GUESS, &statBuffer)) {
        return false;
    }

    auto e = new Archive::Entry();

    if (statBuffer.valid & ZIP_STAT_NAME) {
        e->setFullPath(QString::fromUtf8(statBuffer.name));
    }

    if (e->fullPath(PathFormat::WithTrailingSlash).endsWith(QDir::separator())) {
        e->setProperty("isDirectory", true);
    }

    if (statBuffer.valid & ZIP_STAT_MTIME) {
        e->setProperty("timestamp", QDateTime::fromTime_t(statBuffer.mtime));
    }
    if (statBuffer.valid & ZIP_STAT_SIZE) {
        e->setProperty("size", (qulonglong)statBuffer.size);
    }
    if (statBuffer.valid & ZIP_STAT_COMP_SIZE) {
        e->setProperty("compressedSize", (qlonglong)statBuffer.comp_size);
    }
    if (statBuffer.valid & ZIP_STAT_CRC) {
        if (!e->isDir()) {
            e->setProperty("CRC", QString::number((qulonglong)statBuffer.crc, 16).toUpper());
        }
    }
    if (statBuffer.valid & ZIP_STAT_COMP_METHOD) {
        switch(statBuffer.comp_method) {
            case ZIP_CM_STORE:
                e->setProperty("method", QStringLiteral("Store"));
                emit compressionMethodFound(QStringLiteral("Store"));
                break;
            case ZIP_CM_DEFLATE:
                e->setProperty("method", QStringLiteral("Deflate"));
                emit compressionMethodFound(QStringLiteral("Deflate"));
                break;
            case ZIP_CM_DEFLATE64:
                e->setProperty("method", QStringLiteral("Deflate64"));
                emit compressionMethodFound(QStringLiteral("Deflate64"));
                break;
            case ZIP_CM_BZIP2:
                e->setProperty("method", QStringLiteral("BZip2"));
                emit compressionMethodFound(QStringLiteral("BZip2"));
                break;
            case ZIP_CM_LZMA:
                e->setProperty("method", QStringLiteral("LZMA"));
                emit compressionMethodFound(QStringLiteral("LZMA"));
                break;
            case ZIP_CM_XZ:
                e->setProperty("method", QStringLiteral("XZ"));
                emit compressionMethodFound(QStringLiteral("XZ"));
                break;
        }
    }
    if (statBuffer.valid & ZIP_STAT_ENCRYPTION_METHOD) {
        if (statBuffer.encryption_method != ZIP_EM_NONE) {
            e->setProperty("isPasswordProtected", true);
            switch(statBuffer.encryption_method) {
                case ZIP_EM_TRAD_PKWARE:
                    emit encryptionMethodFound(QStringLiteral("ZipCrypto"));
                    break;
                case ZIP_EM_AES_128:
                    emit encryptionMethodFound(QStringLiteral("AES128"));
                    break;
                case ZIP_EM_AES_192:
                    emit encryptionMethodFound(QStringLiteral("AES192"));
                    break;
                case ZIP_EM_AES_256:
                    emit encryptionMethodFound(QStringLiteral("AES256"));
                    break;
            }
        }
    }

    // Read external attributes, which contains the file permissions.
    zip_uint8_t opsys;
    zip_uint32_t attributes;
    if (zip_file_get_external_attributes(archive, index, ZIP_FL_UNCHANGED, &opsys, &attributes) == -1) {
        emit error(tr("Failed to read metadata for entry: %1"));
        return false;
    }

    // Set permissions.
    switch (opsys) {
    case ZIP_OPSYS_UNIX:
        // Unix permissions are stored in the leftmost 16 bits of the external file attribute.
        e->setProperty("permissions", permissionsToString(attributes >> 16));
        break;
    default:    // TODO: non-UNIX.
        break;
    }

    emit entry(e);
    m_emittedEntries << e;

    return true;
}

bool LibzipPlugin::deleteFiles(const QVector<Archive::Entry*> &files)
{
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), 0, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    qulonglong i = 0;
    for (const Archive::Entry* e : files) {

        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }

        const qlonglong index = zip_name_locate(archive, e->fullPath().toUtf8().constData(), ZIP_FL_ENC_GUESS);
        if (index == -1) {
            emit error(tr("Failed to delete entry: %1"));
            return false;
        }
        if (zip_delete(archive, index) == -1) {
            emit error(tr("Failed to delete entry: %1"));
            return false;
        }
        emit entryRemoved(e->fullPath());
        emit progress(float(++i) / files.size());
    }

    if (zip_close(archive)) {
        emit error(tr("Failed to write archive."));
        return false;
    }
    return true;
}

bool LibzipPlugin::addComment(const QString& comment)
{
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), 0, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    // Set archive comment.
    if (zip_set_archive_comment(archive, comment.toUtf8().constData(), comment.length())) {
        return false;
    }

    if (zip_close(archive)) {
        emit error(tr("Failed to write archive."));
        return false;
    }
    return true;
}

bool LibzipPlugin::testArchive()
{
    int errcode = 0;
    zip_error_t err;

    // Open archive performing extra consistency checks.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), ZIP_CHECKCONS, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        return false;
    }

    // Check CRC-32 for each archive entry.
    const int nofEntries = zip_get_num_entries(archive, 0);
    for (int i = 0; i < nofEntries; i++) {

        if (QThread::currentThread()->isInterruptionRequested()) {
            return false;
        }

        // Get statistic for entry. Used to get entry size.
        zip_stat_t statBuffer;
        if (zip_stat_index(archive, i, 0, &statBuffer) != 0) {
            return false;
        }

        zip_file *zipFile = zip_fopen_index(archive, i, 0);
        std::unique_ptr<uchar[]> buf(new uchar[statBuffer.size]);
        const int len = zip_fread(zipFile, buf.get(), statBuffer.size);
        if (len == -1 || uint(len) != statBuffer.size) {
            return false;
        }
        if (statBuffer.crc != crc32(0, &buf.get()[0], len)) {
            return false;
        }

        emit progress(float(i) / nofEntries);
    }

    zip_close(archive);

    emit testSuccess();
    return true;
}

bool LibzipPlugin::doKill()
{
    return false;
}

bool LibzipPlugin::extractFiles(const QVector<Archive::Entry*> &files, const QString& destinationDirectory, const ExtractionOptions& options)
{
    const bool extractAll = files.isEmpty();
    const bool removeRootNode = options.isDragAndDropEnabled();

    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), ZIP_RDONLY, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    // Set password if known.
    if (!password().isEmpty()) {
        zip_set_default_password(archive, password().toUtf8().constData());
    }

    // Get number of archive entries.
    const qlonglong nofEntries = extractAll ? zip_get_num_entries(archive, 0) : files.size();

    // Extract entries.
    m_overwriteAll = false; // Whether to overwrite all files
    m_skipAll = false; // Whether to skip all files
    if (extractAll) {
        // We extract all entries.
        for (qlonglong i = 0; i < nofEntries; i++) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                break;
            }
            if (!extractEntry(archive,
                              QDir::fromNativeSeparators(QString::fromUtf8(zip_get_name(archive, i, ZIP_FL_ENC_GUESS))),
                              QString(),
                              destinationDirectory,
                              options.preservePaths(),
                              removeRootNode)) {
                return false;
            }
            emit progress(float(i + 1) / nofEntries);
            QString name = QString::fromUtf8(zip_get_name(archive, i, ZIP_FL_ENC_GUESS));
            emit progress_filename(name);
        }
    } else {
        // We extract only the entries in files.
        qulonglong i = 0;
        for (const Archive::Entry* e : files) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                break;
            }
            if (!extractEntry(archive,
                              e->fullPath(),
                              e->rootNode,
                              destinationDirectory,
                              options.preservePaths(),
                              removeRootNode)) {
                return false;
            }
            emit progress(float(++i) / nofEntries);
            emit progress_filename(e->name());
        }
    }

    zip_close(archive);
    return true;
}

bool LibzipPlugin::extractEntry(zip_t *archive, const QString &entry, const QString &rootNode, const QString &destDir, bool preservePaths, bool removeRootNode)
{
    const bool isDirectory = entry.endsWith(QDir::separator());

    // Add trailing slash to destDir if not present.
    QString destDirCorrected(destDir);
    if (!destDir.endsWith(QDir::separator())) {
        destDirCorrected.append(QDir::separator());
    }

    // Remove rootnode if supplied and set destination path.
    QString destination;
    if (preservePaths) {
        if (!removeRootNode || rootNode.isEmpty()) {
            destination = destDirCorrected + entry;
        } else {
            QString truncatedEntry = entry;
            truncatedEntry.remove(0, rootNode.size());
            destination = destDirCorrected + truncatedEntry;
        }
    } else {
        if (isDirectory) {
            return true;
        }
        destination = destDirCorrected + QFileInfo(entry).fileName();
    }

    // Store parent mtime.
    QString parentDir;
    if (isDirectory) {
        QDir pDir = QFileInfo(destination).dir();
        pDir.cdUp();
        parentDir = pDir.path();
    } else {
        parentDir = QFileInfo(destination).path();
    }
    // For top-level items, don't restore parent dir mtime.
    const bool restoreParentMtime = (parentDir + QDir::separator() != destDirCorrected);

    time_t parent_mtime;
    if (restoreParentMtime) {
        parent_mtime = QFileInfo(parentDir).lastModified().toMSecsSinceEpoch() / 1000;
    }

    // Create parent directories for files. For directories create them.
    if (!QDir().mkpath(QFileInfo(destination).path())) {
        emit error(tr("Failed to create directory: %1"));
        return false;
    }

    // Get statistic for entry. Used to get entry size and mtime.
    zip_stat_t statBuffer;
    if (zip_stat(archive, entry.toUtf8().constData(), 0, &statBuffer) != 0) {
        if (isDirectory && zip_error_code_zip(zip_get_error(archive)) == ZIP_ER_NOENT) {
            return true;
        }
        return false;
    }

    if (!isDirectory) {

        // Handle existing destination files.
        QString renamedEntry = entry;
        while (!m_overwriteAll && QFileInfo::exists(destination)) {
            if (m_skipAll) {
                return true;
            } else {
                OverwriteQuery query(renamedEntry);
                emit userQuery(&query);
                query.waitForResponse();

                if (query.responseCancelled()) {
                    emit cancelled();
                    return false;
                } else if (query.responseSkip()) {
                    return true;
                } else if (query.responseAutoSkip()) {
                    m_skipAll = true;
                    return true;
                } else if (query.responseRename()) {
                    const QString newName(query.newFilename());
                    destination = QFileInfo(destination).path() + QDir::separator() + QFileInfo(newName).fileName();
                    renamedEntry = QFileInfo(entry).path() + QDir::separator() + QFileInfo(newName).fileName();
                } else if (query.responseOverwriteAll()) {
                    m_overwriteAll = true;
                    break;
                } else if (query.responseOverwrite()) {
                    break;
                }
            }
        }

        // Handle password-protected files.
        zip_file *zipFile = nullptr;
        bool firstTry = true;
        while (!zipFile) {
            zipFile = zip_fopen(archive, entry.toUtf8().constData(), 0);
            if (zipFile) {
                break;
            } else if (zip_error_code_zip(zip_get_error(archive)) == ZIP_ER_NOPASSWD ||
                       zip_error_code_zip(zip_get_error(archive)) == ZIP_ER_WRONGPASSWD) {
                PasswordNeededQuery query(filename(), !firstTry);
                emit userQuery(&query);
                query.waitForResponse();

                if (query.responseCancelled()) {
                    emit cancelled();
                    return false;
                }
                setPassword(query.password());

                if (zip_set_default_password(archive, password().toUtf8().constData())) {
                }
                firstTry = false;
            } else {
                emit error(tr("Failed to open '%1':<nl/>%2"));
                return false;
            }
        }

        QFile file(destination);
        if (!file.open(QIODevice::WriteOnly)) {
            emit error(tr("Failed to open file for writing: %1"));
            return false;
        }

        QDataStream out(&file);

        // Write archive entry to file. We use a read/write buffer of 1000 chars.
        qulonglong sum = 0;
        char buf[1000];
        while (sum != statBuffer.size) {
            const auto readBytes = zip_fread(zipFile, buf, 1000);
            if (readBytes < 0) {
                emit error(tr("Failed to read data for entry: %1"));
                return false;
            }
            if (out.writeRawData(buf, readBytes) != readBytes) {
                emit error(tr("Failed to write data for entry: %1"));
                return false;
            }

            sum += readBytes;
        }

        const auto index = zip_name_locate(archive, entry.toUtf8().constData(), ZIP_FL_ENC_GUESS);
        if (index == -1) {
            emit error(tr("Failed to locate entry: %1"));
            return false;
        }

        zip_uint8_t opsys;
        zip_uint32_t attributes;
        if (zip_file_get_external_attributes(archive, index, ZIP_FL_UNCHANGED, &opsys, &attributes) == -1) {
            emit error(tr("Failed to read metadata for entry: %1"));
            return false;
        }

        // Inspired by fuse-zip source code: fuse-zip/lib/fileNode.cpp
        switch (opsys) {
        case ZIP_OPSYS_UNIX:
            // Unix permissions are stored in the leftmost 16 bits of the external file attribute.
//            file.setPermissions(KIO::convertPermissions(attributes >> 16)); //TODO_DS
            break;
        default:    // TODO: non-UNIX.
            break;
        }

        file.close();
    }

    // Set mtime for entry.
    utimbuf times;
    times.modtime = statBuffer.mtime;
    if (utime(destination.toUtf8().constData(), &times) != 0) {
    }

    if (restoreParentMtime) {
        // Restore mtime for parent dir.
        times.modtime = parent_mtime;
        if (utime(parentDir.toUtf8().constData(), &times) != 0) {
        }
    }

    return true;
}

bool LibzipPlugin::moveFiles(const QVector<Archive::Entry*> &files, Archive::Entry *destination, const CompressionOptions &options)
{
    Q_UNUSED(options)
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), 0, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    QStringList filePaths = entryFullPaths(files);
    filePaths.sort();
    const QStringList destPaths = entryPathsFromDestination(filePaths, destination, entriesWithoutChildren(files).count());

    int i;
    for (i = 0; i < filePaths.size(); ++i) {

        const int index = zip_name_locate(archive, filePaths.at(i).toUtf8().constData(), ZIP_FL_ENC_GUESS);
        if (index == -1) {
            emit error(tr("Failed to move entry: %1"));
            return false;
        }

        if (zip_file_rename(archive, index, destPaths.at(i).toUtf8().constData(), ZIP_FL_ENC_GUESS) == -1) {
            emit error(tr("Failed to move entry: %1"));
            return false;
        }

        emit entryRemoved(filePaths.at(i));
        emitEntryForIndex(archive, index);
        emit progress(i/filePaths.count());
    }
    if (zip_close(archive)) {
        emit error(tr("Failed to write archive."));
        return false;
    }

    return true;
}

bool LibzipPlugin::copyFiles(const QVector<Archive::Entry*> &files, Archive::Entry *destination, const CompressionOptions &options)
{
    Q_UNUSED(options)
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(filename()).constData(), 0, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        emit error(tr("Failed to open archive: %1"));
        return false;
    }

    const QStringList filePaths = entryFullPaths(files);
    const QStringList destPaths = entryPathsFromDestination(filePaths, destination, 0);

    int i;
    for (i = 0; i < filePaths.size(); ++i) {

        QString dest = destPaths.at(i);

        if (dest.endsWith(QDir::separator())) {
            if (zip_dir_add(archive, dest.toUtf8().constData(), ZIP_FL_ENC_GUESS) == -1) {
                // If directory already exists in archive, we get an error.
                continue;
            }
        }

        const int srcIndex = zip_name_locate(archive, filePaths.at(i).toUtf8().constData(), ZIP_FL_ENC_GUESS);
        if (srcIndex == -1) {
            emit error(tr("Failed to copy entry: %1"));
            return false;
        }

        zip_source_t *src = zip_source_zip(archive, archive, srcIndex, 0, 0, -1);
        if (!src) {
            return false;
        }

        const int destIndex = zip_file_add(archive, dest.toUtf8().constData(), src, ZIP_FL_ENC_GUESS | ZIP_FL_OVERWRITE);
        if (destIndex == -1) {
            zip_source_free(src);
            emit error(tr("Failed to add entry: %1"));
            return false;
        }

        // Get permissions from source entry.
        zip_uint8_t opsys;
        zip_uint32_t attributes;
        if (zip_file_get_external_attributes(archive, srcIndex, ZIP_FL_UNCHANGED, &opsys, &attributes) == -1) {
            emit error(tr("Failed to read metadata for entry: %1"));
            return false;
        }

        // Set permissions on dest entry.
        if (zip_file_set_external_attributes(archive, destIndex, ZIP_FL_UNCHANGED, opsys, attributes) != 0) {
            emit error(tr("Failed to set metadata for entry: %1"));
            return false;
        }
    }

    // Register the callback function to get progress feedback.
    zip_register_progress_callback_with_state(archive, 0.001, progressCallback, nullptr, this);

    if (zip_close(archive)) {
        emit error(tr("Failed to write archive."));
        return false;
    }

    // List the archive to update the model.
    m_listAfterAdd = true;
    list();


    return true;
}

QString LibzipPlugin::permissionsToString(const mode_t &perm)
{
    QString modeval;
    if ((perm & S_IFMT) == S_IFDIR) {
        modeval.append(QLatin1Char('d'));
    } else if ((perm & S_IFMT) == S_IFLNK) {
        modeval.append(QLatin1Char('l'));
    } else {
        modeval.append(QLatin1Char('-'));
    }
    modeval.append((perm & S_IRUSR) ? QLatin1Char('r') : QLatin1Char('-'));
    modeval.append((perm & S_IWUSR) ? QLatin1Char('w') : QLatin1Char('-'));
    if ((perm & S_ISUID) && (perm & S_IXUSR)) {
        modeval.append(QLatin1Char('s'));
    } else if ((perm & S_ISUID)) {
        modeval.append(QLatin1Char('S'));
    } else if ((perm & S_IXUSR)) {
        modeval.append(QLatin1Char('x'));
    } else {
        modeval.append(QLatin1Char('-'));
    }
    modeval.append((perm & S_IRGRP) ? QLatin1Char('r') : QLatin1Char('-'));
    modeval.append((perm & S_IWGRP) ? QLatin1Char('w') : QLatin1Char('-'));
    if ((perm & S_ISGID) && (perm & S_IXGRP)) {
        modeval.append(QLatin1Char('s'));
    } else if ((perm & S_ISGID)) {
        modeval.append(QLatin1Char('S'));
    } else if ((perm & S_IXGRP)) {
        modeval.append(QLatin1Char('x'));
    } else {
        modeval.append(QLatin1Char('-'));
    }
    modeval.append((perm & S_IROTH) ? QLatin1Char('r') : QLatin1Char('-'));
    modeval.append((perm & S_IWOTH) ? QLatin1Char('w') : QLatin1Char('-'));
    if ((perm & S_ISVTX) && (perm & S_IXOTH)) {
        modeval.append(QLatin1Char('t'));
    } else if ((perm & S_ISVTX)) {
        modeval.append(QLatin1Char('T'));
    } else if ((perm & S_IXOTH)) {
        modeval.append(QLatin1Char('x'));
    } else {
        modeval.append(QLatin1Char('-'));
    }
    return modeval;
}

//#include "libzipplugin.moc"
