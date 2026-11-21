#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>
#include <QVariantList>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantList lanDevices READ lanDevices NOTIFY lanDevicesChanged)

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);
    Q_INVOKABLE int indexForUuid(const QString& uuid) const;
    Q_INVOKABLE QString nameAt(int index) const;
    Q_INVOKABLE QString uuidAt(int index) const;
    Q_INVOKABLE QString detailsAt(int index) const;
    QVariantList lanDevices() const;

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    Q_INVOKABLE QVariantList getConnectionAddressesForComputer(int computerIndex) const;

    Q_INVOKABLE bool hasMultipleConnectionAddresses(int computerIndex) const;

    Q_INVOKABLE bool setActiveAddressForComputer(int computerIndex, QString address, int port);

    // Undo a setActiveAddressForComputer() pin and go back to automatic selection.
    Q_INVOKABLE bool resetToAutomaticAddressForComputer(int computerIndex);

signals:
    void lanDevicesChanged();
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
