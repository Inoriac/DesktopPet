#ifndef DESKTOP_PET_EMBEDDING_PROVIDER_H
#define DESKTOP_PET_EMBEDDING_PROVIDER_H

#include <QString>
#include <QVector>

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;

    virtual QString modelName() const = 0;
    virtual int dimension() const = 0;
    virtual QVector<float> embed(const QString& text) = 0;
};

#endif // DESKTOP_PET_EMBEDDING_PROVIDER_H
