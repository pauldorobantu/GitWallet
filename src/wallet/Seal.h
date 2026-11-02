#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// What a password locks with: Argon2id makes a key of it, slowly and with a lot of memory, so that
// guessing costs that much per guess; AES-256-GCM locks with that key, so that a wrong key and a
// changed byte both fail. How the key was made goes along in the clear, and the check covers it.
namespace wallet::seal {

struct Cost {
    quint32 memoryKib = 256 * 1024;
    quint32 passes = 3;
    quint32 lanes = 1;
    QByteArray salt;
};

constexpr qsizetype kKeyBytes = 32;

// A new salt, at what a new key costs now: about a second on a PC of today.
Cost newCost();
// For the checks of this program, whose runs would otherwise take minutes.
void setCostForTest(quint32 memoryKib, quint32 passes);

// As typed, whichever way the keyboard composes its letters. Empty for nothing.
QByteArray passwordBytes(const QString& password);
// The key of those bytes at that cost; empty if it could not be made, which only too little memory
// does. Any thread. Takes as long as `cost` says.
QByteArray makeKey(const QByteArray& password, const Cost& cost);

// `plain` locked under `key`, with a new nonce every time, as an object saying all that opening it
// takes but the password. `format` names what it is, and is part of what the check covers.
QJsonObject lock(const QByteArray& key, const Cost& cost, const QByteArray& plain, const QString& format);

// How that object says its key was made. False when it is not such an object, or asks for more than
// any honest one would: a file nobody knows the source of does not get to take all the memory there is.
bool costOf(const QJsonObject& sealed, Cost* cost);
// What it holds. False for a wrong key, and for a byte changed anywhere in it.
bool open(const QByteArray& key, const QJsonObject& sealed, QByteArray* plain);

// Best effort: what is in memory is gone once it is not needed.
void wipe(QByteArray& bytes);

} // namespace wallet::seal
