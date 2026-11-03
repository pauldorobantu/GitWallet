#include "wallet/Seal.h"

#include <botan/aead.h>
#include <botan/exceptn.h>
#include <botan/pwdhash.h>
#include <botan/secmem.h>
#include <botan/system_rng.h>

#include <QJsonValue>

#include <cmath>
#include <exception>
#include <memory>
#include <string_view>

#include <windows.h>

namespace wallet::seal {

namespace {

constexpr qsizetype kSaltBytes = 16;
constexpr qsizetype kNonceBytes = 12;
constexpr int kVersion = 1;
// A key is made with less than this, now and for a long time to come.
constexpr double kMostMemoryKib = 2.0 * 1024 * 1024;
constexpr double kMostPasses = 16;
constexpr double kMostLanes = 16;

quint32 gTestMemoryKib = 0;
quint32 gTestPasses = 0;

QByteArray randomBytes(qsizetype count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    Botan::system_rng().randomize(reinterpret_cast<uint8_t*>(bytes.data()), size_t(bytes.size()));
    return bytes;
}

// All that is in the clear, in one order, so that the check covers it.
QByteArray headerOf(const QString& format, int version, const Cost& cost, const QByteArray& nonce)
{
    return format.toUtf8() + '|' + QByteArray::number(version) + "|argon2id|" + QByteArray::number(cost.memoryKib) + '|' + QByteArray::number(cost.passes) + '|'
           + QByteArray::number(cost.lanes) + '|' + cost.salt.toHex() + "|aes-256-gcm|" + nonce.toHex();
}

bool wholeNumber(const QJsonValue& value, double most, quint32* out)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!(number >= 1.0 && number <= most) || number != std::floor(number))
        return false;
    *out = quint32(number);
    return true;
}

} // namespace

Cost newCost()
{
    Cost cost;
    if (gTestMemoryKib > 0) {
        cost.memoryKib = gTestMemoryKib;
        cost.passes = gTestPasses;
    }
    cost.salt = randomBytes(kSaltBytes);
    return cost;
}

void setCostForTest(quint32 memoryKib, quint32 passes)
{
    gTestMemoryKib = memoryKib;
    gTestPasses = passes;
}

QByteArray passwordBytes(const QString& password)
{
    return password.normalized(QString::NormalizationForm_KC).toUtf8();
}

QByteArray makeKey(const QByteArray& password, const Cost& cost)
{
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    try {
        const std::unique_ptr<Botan::PasswordHashFamily> family = Botan::PasswordHashFamily::create_or_throw("Argon2id");
        const std::unique_ptr<Botan::PasswordHash> hash = family->from_params(cost.memoryKib, cost.passes, cost.lanes);
        hash->derive_key(reinterpret_cast<uint8_t*>(key.data()), size_t(key.size()), password.constData(), size_t(password.size()),
                         reinterpret_cast<const uint8_t*>(cost.salt.constData()), size_t(cost.salt.size()));
    } catch (const std::exception&) {
        wipe(key);
        return {};
    }
    return key;
}

QJsonObject lock(const QByteArray& key, const Cost& cost, const QByteArray& plain, const QString& format)
{
    const QByteArray nonce = randomBytes(kNonceBytes);
    const QByteArray header = headerOf(format, kVersion, cost, nonce);
    Botan::secure_vector<uint8_t> buffer(plain.cbegin(), plain.cend());
    const std::unique_ptr<Botan::AEAD_Mode> mode = Botan::AEAD_Mode::create_or_throw("AES-256/GCM", Botan::Cipher_Dir::Encryption);
    mode->set_key(reinterpret_cast<const uint8_t*>(key.constData()), size_t(key.size()));
    mode->set_associated_data(reinterpret_cast<const uint8_t*>(header.constData()), size_t(header.size()));
    mode->start(reinterpret_cast<const uint8_t*>(nonce.constData()), size_t(nonce.size()));
    mode->finish(buffer);

    QJsonObject kdf;
    kdf.insert(QStringLiteral("name"), QStringLiteral("argon2id"));
    kdf.insert(QStringLiteral("memoryKib"), qint64(cost.memoryKib));
    kdf.insert(QStringLiteral("passes"), qint64(cost.passes));
    kdf.insert(QStringLiteral("lanes"), qint64(cost.lanes));
    kdf.insert(QStringLiteral("salt"), QString::fromLatin1(cost.salt.toHex()));
    QJsonObject sealed;
    sealed.insert(QStringLiteral("format"), format);
    sealed.insert(QStringLiteral("version"), kVersion);
    sealed.insert(QStringLiteral("kdf"), kdf);
    sealed.insert(QStringLiteral("cipher"), QStringLiteral("aes-256-gcm"));
    sealed.insert(QStringLiteral("nonce"), QString::fromLatin1(nonce.toHex()));
    sealed.insert(QStringLiteral("body"), QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(buffer.data()), qsizetype(buffer.size())).toBase64()));
    return sealed;
}

bool costOf(const QJsonObject& sealed, Cost* cost)
{
    const QJsonObject kdf = sealed.value(QStringLiteral("kdf")).toObject();
    if (kdf.value(QStringLiteral("name")).toString() != QLatin1String("argon2id"))
        return false;
    Cost read;
    if (!wholeNumber(kdf.value(QStringLiteral("memoryKib")), kMostMemoryKib, &read.memoryKib) || !wholeNumber(kdf.value(QStringLiteral("passes")), kMostPasses, &read.passes)
        || !wholeNumber(kdf.value(QStringLiteral("lanes")), kMostLanes, &read.lanes))
        return false;
    read.salt = QByteArray::fromHex(kdf.value(QStringLiteral("salt")).toString().toLatin1());
    if (read.salt.size() != kSaltBytes)
        return false;
    *cost = read;
    return true;
}

bool open(const QByteArray& key, const QJsonObject& sealed, QByteArray* plain)
{
    Cost cost;
    if (key.size() != kKeyBytes || !costOf(sealed, &cost) || sealed.value(QStringLiteral("cipher")).toString() != QLatin1String("aes-256-gcm"))
        return false;
    const QByteArray nonce = QByteArray::fromHex(sealed.value(QStringLiteral("nonce")).toString().toLatin1());
    const QByteArray body = QByteArray::fromBase64(sealed.value(QStringLiteral("body")).toString().toLatin1());
    if (nonce.size() != kNonceBytes || body.isEmpty())
        return false;
    const QByteArray header = headerOf(sealed.value(QStringLiteral("format")).toString(), sealed.value(QStringLiteral("version")).toInt(), cost, nonce);
    Botan::secure_vector<uint8_t> buffer(body.cbegin(), body.cend());
    try {
        const std::unique_ptr<Botan::AEAD_Mode> mode = Botan::AEAD_Mode::create_or_throw("AES-256/GCM", Botan::Cipher_Dir::Decryption);
        mode->set_key(reinterpret_cast<const uint8_t*>(key.constData()), size_t(key.size()));
        mode->set_associated_data(reinterpret_cast<const uint8_t*>(header.constData()), size_t(header.size()));
        mode->start(reinterpret_cast<const uint8_t*>(nonce.constData()), size_t(nonce.size()));
        mode->finish(buffer);
    } catch (const std::exception&) {
        return false;
    }
    *plain = QByteArray(reinterpret_cast<const char*>(buffer.data()), qsizetype(buffer.size()));
    return true;
}

void wipe(QByteArray& bytes)
{
    if (!bytes.isEmpty())
        SecureZeroMemory(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}

} // namespace wallet::seal
