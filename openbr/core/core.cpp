/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 The MITRE Corporation                                      *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <openbr/openbr_plugin.h>

#include "bee.h"
#include "common.h"
#include "qtutils.h"
#include "../plugins/openbr_internal.h"

namespace br {

struct AlgorithmCore
{
    QSharedPointer<Transform> transform;
    QSharedPointer<Distance> distance;

    QString transformString;
    QString distanceString;

    AlgorithmCore(const QString &name)
    {
        this->name = name;
        init(name);
    }

    bool isClassifier() const
    {
        return distance.isNull();
    }

    void train(const File &input, const QString &model)
    {
        qDebug("Training on %s%s", qPrintable(input.flat()),
               model.isEmpty() ? "" : qPrintable(" to " + model));

        QScopedPointer<Transform> trainingWrapper(Transform::make("DirectStream([Identity], readMode=DistributeFrames)", NULL));

        CompositeTransform * downcast = dynamic_cast<CompositeTransform *>(trainingWrapper.data());
        if (downcast == NULL)
            qFatal("downcast failed?");
        downcast->transforms[0] = this->transform.data();

        downcast->init();

        TemplateList data(TemplateList::fromGallery(input));

        if (transform.isNull()) qFatal("Null transform.");
        qDebug("%d Training Files", data.size());

        Globals->startTime.start();

        qDebug("Training Enrollment");
        downcast->train(data);

        if (!distance.isNull()) {
            if (Globals->crossValidate > 0)
                for (int i=data.size()-1; i>=0; i--) if (data[i].file.get<bool>("allPartitions",false)) data.removeAt(i);

            qDebug("Projecting Enrollment");
            downcast->projectUpdate(data,data);

            qDebug("Training Comparison");
            distance->train(data);
        }

        if (!model.isEmpty()) {
            qDebug("Storing %s", qPrintable(QFileInfo(model).fileName()));
            store(model);
        }

        qDebug("Training Time: %s", qPrintable(QtUtils::toTime(Globals->startTime.elapsed()/1000.0f)));
    }

    void store(const QString &model) const
    {
        // Create stream
        QByteArray data;
        QDataStream out(&data, QFile::WriteOnly);

        // Serialize algorithm to stream
        out << name;
        transform->store(out);
        const bool hasComparer = !distance.isNull();
        out << hasComparer;
        if (hasComparer) distance->store(out);

        // Compress and save to file
        QtUtils::writeFile(model, data, -1);
    }

    void load(const QString &model)
    {
        // Load from file and decompress
        QByteArray data;
        QtUtils::readFile(model, data, true);

        // Create stream
        QDataStream in(&data, QFile::ReadOnly);

        // Load algorithm
        in >> name; init(Globals->abbreviations.contains(name) ? Globals->abbreviations[name] : name);
        transform->load(in);
        bool hasDistance; in >> hasDistance;
        if (hasDistance) distance->load(in);
    }

    File getMemoryGallery(const File &file) const
    {
        return name + file.baseName() + file.hash() + ".mem";
    }

    FileList enroll(File input, File gallery = File())
    {
        FileList files;

        qDebug("Enrolling %s%s", qPrintable(input.flat()),
               gallery.isNull() ? "" : qPrintable(" to " + gallery.flat()));

        if (gallery.name.isEmpty()) {
            if (input.name.isEmpty()) return FileList();
            else                      gallery = getMemoryGallery(input);
        }
        TemplateList data(TemplateList::fromGallery(input));

        bool multiProcess = Globals->file.getBool("multiProcess", false);

        if (gallery.contains("append"))
        {
            // Remove any templates which are already in the gallery
            QScopedPointer<Gallery> g(Gallery::make(gallery));
            files = g->files();
            QSet<QString> nameSet = QSet<QString>::fromList(files.names());
            for (int i = data.size() - 1; i>=0; i--) {
                if (nameSet.contains(data[i].file.name))
                {
                    data.removeAt(i);
                }
            }
        }

        if (data.empty())
            return files;

        // Store steps for ProgressCounter
        Globals->currentStep = 0;
        Globals->totalSteps = data.length();

        QScopedPointer<Transform> basePipe;

        if (!multiProcess)
        {
            QString pipeDesc = "Identity+GalleryOutput("+gallery.flat()+")+ProgressCounter("+QString::number(data.length())+")+Discard";
            basePipe.reset(Transform::make(pipeDesc,NULL));
            CompositeTransform * downcast = dynamic_cast<CompositeTransform *>(basePipe.data());
            if (downcast == NULL)
                qFatal("downcast failed?");

            // replace that placeholder with the current algorithm
            downcast->transforms[0] = this->transform.data();

            // call init on the pipe to collapse the algorithm (if its top level is a pipe)
            downcast->init();
        }
        else
        {
            QString pipeDesc = "ProcessWrapper("+transformString+")"+"+GalleryOutput("+gallery.flat()+")+ProgressCounter("+QString::number(data.length())+")+Discard";
            basePipe.reset(Transform::make(pipeDesc,NULL));
        }

        // Next, we make a Stream (with placeholder transform)
        QString streamDesc = "Stream(Identity, readMode=DistributeFrames)";
        QScopedPointer<Transform> baseStream(Transform::make(streamDesc, NULL));
        WrapperTransform * wrapper = dynamic_cast<WrapperTransform *> (baseStream.data());

        // replace that placeholder with the pipe we built
        wrapper->transform = basePipe.data();

        // and get the final stream's stages by reinterpreting the pipe. Perfectly straightforward.
        wrapper->init();

        Globals->startTime.start();

        wrapper->projectUpdate(data,data);

        files.append(data.files());

        return files;
    }

    void enroll(TemplateList &data)
    {
        if (transform.isNull()) qFatal("Null transform.");
        data >> *transform;
    }

    void retrieveOrEnroll(const File &file, QScopedPointer<Gallery> &gallery, FileList &galleryFiles)
    {
        if (!file.getBool("enroll") && (QStringList() << "gal" << "mem" << "template").contains(file.suffix())) {
            // Retrieve it
            gallery.reset(Gallery::make(file));
            galleryFiles = gallery->files();
        } else {
            // Was it already enrolled in memory?
            gallery.reset(Gallery::make(getMemoryGallery(file)));
            galleryFiles = gallery->files();
            if (!galleryFiles.isEmpty()) return;

            // Enroll it
            enroll(file);
            gallery.reset(Gallery::make(getMemoryGallery(file)));
            galleryFiles = gallery->files();
        }
    }

    void pairwiseCompare(File targetGallery, File queryGallery, File output)
    {
        qDebug("Pairwise comparing %s and %s%s", qPrintable(targetGallery.flat()),
               qPrintable(queryGallery.flat()),
               output.isNull() ? "" : qPrintable(" to " + output.flat()));

        if (distance.isNull()) qFatal("Null distance.");

        if (queryGallery == ".") queryGallery = targetGallery;

        QScopedPointer<Gallery> t, q;
        FileList targetFiles, queryFiles;
        retrieveOrEnroll(targetGallery, t, targetFiles);
        retrieveOrEnroll(queryGallery, q, queryFiles);

        if (t->files().length() != q->files().length() )
            qFatal("Dimension mismatch in pairwise compare");

        TemplateList queries = q->read();
        TemplateList targets = t->read();

        // Use a single file for one of the dimensions so that the output makes the right size file
        FileList dummyTarget;
        dummyTarget.append(targets[0]);
        QScopedPointer<Output> realOutput(Output::make(output, dummyTarget, queryFiles));

        realOutput->set_blockRows(INT_MAX);
        realOutput->set_blockCols(INT_MAX);
        realOutput->setBlock(0,0);
        for (int i=0; i < queries.length(); i++)
        {
            float res = distance->compare(queries[i], targets[i]);
            realOutput->setRelative(res, 0,i);
        }
    }

    void deduplicate(const File &inputGallery, const File &outputGallery, const float threshold)
    {
        qDebug("Deduplicating %s to %s with a score threshold of %f", qPrintable(inputGallery.flat()), qPrintable(outputGallery.flat()), threshold);

        if (distance.isNull()) qFatal("Null distance.");

        QScopedPointer<Gallery> i;
        FileList inputFiles;
        retrieveOrEnroll(inputGallery, i, inputFiles);

        TemplateList t = i->read();

        Output *o = Output::make(QString("buffer.tail[selfSimilar,threshold=%1,atLeast=0]").arg(QString::number(threshold)),inputFiles,inputFiles);

        // Compare to global tail output
        distance->compare(t,t,o);

        delete o;

        QString buffer(Globals->buffer);

        QStringList tail = buffer.split("\n");

        // Remove header
        tail.removeFirst();

        QStringList toRemove;
        foreach(const QString &s, tail)
            toRemove.append(s.split(',').at(1));

        QSet<QString> duplicates = QSet<QString>::fromList(toRemove);

        QStringList fileNames = inputFiles.names();

        QList<int> indices;
        foreach(const QString &d, duplicates)
            indices.append(fileNames.indexOf(d));

        std::sort(indices.begin(),indices.end(),std::greater<float>());

        qDebug("\n%d duplicates removed.", indices.size());

        for (int i=0; i<indices.size(); i++)
            inputFiles.removeAt(indices[i]);

        QScopedPointer<Gallery> og(Gallery::make(outputGallery));

        og->writeBlock(inputFiles);
    }

    void compare(File targetGallery, File queryGallery, File output)
    {
        qDebug("Comparing %s and %s%s", qPrintable(targetGallery.flat()),
               qPrintable(queryGallery.flat()),
               output.isNull() ? "" : qPrintable(" to " + output.flat()));

        // Escape hatch for distances that need to operate directly on the gallery files
        if (distance->compare(targetGallery, queryGallery, output))
            return;

        // Are we comparing the same gallery against itself?
        bool selfCompare = targetGallery == queryGallery;

        // Should we use multiple processes to do enrollment/comparison? If not, we just do multi-threading.
        bool multiProcess = Globals->file.getBool("multiProcess", false);

        // In comparing two galleries, we will keep the smaller one in memory, and load the larger one
        // incrementally. If the gallery set is larger than the probe set, we operate in transpose mode
        // i.e. we must transpose our output, to still write the output matrix in row-major order.
        bool transposeMode = false;

        // Is the larger gallery already enrolled? If not, we will enroll those images in-line with their
        // comparison against the smaller gallery (which will be enrolled, and stored in memory).
        bool needEnrollRows = false;




        if (output.exists() && output.get<bool>("cache", false)) return;
        if (queryGallery == ".") queryGallery = targetGallery;


        // To decide which gallery is larger, we need to read both, but at this point we just want the
        // metadata, and don't need the enrolled matrices.
        FileList targetMetadata;
        FileList queryMetadata;

        // Emptyread reads a gallery, and discards any matrices present, keeping only the metadata.
        emptyRead(targetGallery, targetMetadata);
        emptyRead(queryGallery, queryMetadata);


        // Is the target or query set larger? We will use the larger as the rows of our comparison matrix (and transpose the output if necessary)
        transposeMode = targetMetadata.size() > queryMetadata.size();

        File rowGallery = queryGallery;
        File colGallery = targetGallery;
        int rowSize = queryMetadata.size();

        if (transposeMode)
        {
            rowGallery = targetGallery;
            colGallery = queryGallery;
            rowSize = targetMetadata.size();
        }


        // Is the column gallery already enrolled? We keep the enrolled column gallery in memory, and in multi-process
        // mode, every worker process retains a copy of this gallery in memory. When not in multi-process mode, we can
        // simple make sure the enrolled data is stored in a memGallery, but in multi-process mode we save the enrolled
        // data to disk (as a .gal file) so that each worker process can read it without re-doing enrollment.
        File colEnrolledGallery = colGallery;
        QString targetExtension = multiProcess ? "gal" : "mem";

        // If the column gallery is not already of the appropriate type, we need to do something
        if (colGallery.suffix() != targetExtension)
        {
            // Build the name of a gallery containing the enrolled data, of the appropriate type.
            colEnrolledGallery = colGallery.baseName() + colGallery.hash() + (multiProcess ? ".gal" : ".mem");

            // Check if we have to do real enrollment, and not just convert the gallery's type.
            if (!(QStringList() << "gal" << "template" << "mem").contains(colGallery.suffix()))
            {
                enroll(colGallery, colEnrolledGallery);
            }
            // If the gallery does have enrolled templates, but is not the right type, we do a simple
            // type conversion for it.
            else
            {
                QScopedPointer<Gallery> readColGallery(Gallery::make(colGallery));
                TemplateList templates = readColGallery->read();
                QScopedPointer<Gallery> enrolledColOutput(Gallery::make(colEnrolledGallery));
                enrolledColOutput->writeBlock(templates);
            }
        }

        // We have handled the column gallery, now decide whehter or not we have to enroll the row gallery.
        if (selfCompare)
        {
            // For self-comparisons, we just use the already enrolled column set.
            rowGallery = colEnrolledGallery;
        }
        // Otherwise, we will need to enroll the row set. Since the actual comparison is defined via a transform
        // which compares incoming templates against a gallery, we will handle enrollment of the row set by simply
        // building a transform that does enrollment (using the current algorithm), then does the comparison in one
        // step. This way, we don't have to retain the complete enrolled row gallery in memory, or on disk.
        else if(!(QStringList() << "gal" << "mem" << "template").contains(rowGallery.suffix()))
        {
            needEnrollRows = true;
        }

        // At this point, we have decided how we will structure the comparison (either in transpose mode, or not), 
        // and have the column gallery enrolled, and have decided whether or not we need to enroll the row gallery.
        // From this point, we will build a single algorithm that (optionally) does enrollment, then does comparisons
        // and output, optionally using ProcessWrapper to do the enrollment and comparison in separate processes.
        //
        // There are two main components to this algorithm. The first is the (optional) enrollment and then the
        // comparison step (built from a GalleryCompare transform), and the second is the sequential matrix output and
        // progress counting step.
        // After the base algorithm is built, the whole thing will be run in a stream, so that I/O can be handled sequentially.



        // The actual comparison step is done by a GalleryCompare transform, which has a Distance, and a gallery as data.
        // Incoming templates are compared against the templates in the gallery, and the output is the resulting score
        // vector.
        QString compareRegionDesc = "GalleryCompare("+Globals->algorithm + "," + colEnrolledGallery.flat() + ")";


        QScopedPointer<Transform> compareRegion;
        // If we need to enroll the row set, we add the current algorithm's enrollment transform before the
        // GalleryCompare in a pipe.
        if (needEnrollRows)
        {
            if (!multiProcess)
            {
                compareRegionDesc = "Identity+" + compareRegionDesc;
                compareRegion.reset(Transform::make(compareRegionDesc,NULL));
                CompositeTransform * downcast = dynamic_cast<CompositeTransform *> (compareRegion.data());
                if (downcast == NULL)
                    qFatal("Pipe downcast failed in compare");

                downcast->transforms[0] = this->transform.data();
                downcast->init();
            }
            else
            {
                compareRegionDesc = "ProcessWrapper(" + this->transformString + "+" + compareRegionDesc + ")";
                compareRegion.reset(Transform::make(compareRegionDesc, NULL));
            }
        }
        else {
            if (multiProcess)
                compareRegionDesc = "ProcessWrapper(" + compareRegionDesc + ")";
            compareRegion.reset(Transform::make(compareRegionDesc,NULL));
        }

        // At this point, compareRegion is a transform, which optionally does enrollment, then compares the row
        // set against the column set. If in multi-process mode, the enrollment and comparison are wrapped in a 
        // ProcessWrapper transform, and will be transparently run in multiple processes.
        compareRegion->init();


        // We also need to add Output and progress counting to the algorithm we are building, so we will assign them to
        // two stages of a pipe.
        QString joinDesc = "Identity+Identity";
        QScopedPointer<Transform> join(Transform::make(joinDesc, NULL));

        // The output transform takes the metadata memGalleries we set up previously as input, along with the
        // output specification we were passed. Gallery metadata is necessary for some Outputs to function correctly.
        QString outputString = output.flat().isEmpty() ? "Empty" : output.flat();
        QString outputRegionDesc = "Output("+ outputString +"," + targetGallery.flat() +"," + queryGallery.flat() + ","+ QString::number(transposeMode ? 1 : 0) + ")";
        // The ProgressCounter transform will simply provide a display about the number of rows completed.
        outputRegionDesc += "+ProgressCounter("+QString::number(rowSize)+")+Discard";
        QScopedPointer<Transform> outputTform(Transform::make(outputRegionDesc, NULL));

        // Assign the comparison transform we previously built, and the output transform  we just built to
        // two stages of a pipe.
        CompositeTransform * downcast = dynamic_cast<CompositeTransform *> (join.data());
        downcast->transforms[0] = compareRegion.data();
        downcast->transforms[1] = outputTform.data();

        // With this, we have set up a transform which (optionally) enrolls templates, compares them
        // against a gallery, and outputs them.
        join->init();

        // Now, we will give that base transform to a stream, which will incrementally read the row gallery
        // and pass the transforms it reads through the base algorithm.
        QString streamDesc = "Stream(Identity, readMode=StreamGallery)";
        QScopedPointer<Transform> streamBase(Transform::make(streamDesc, NULL));
        WrapperTransform * streamWrapper = dynamic_cast<WrapperTransform *> (streamBase.data());
        streamWrapper->transform = join.data();

        // The transform we will use is now complete.
        streamWrapper->init();

        // We set up a template containing the rowGallery we want to compare. 
        TemplateList rowGalleryTemplate;
        rowGalleryTemplate.append(Template(rowGallery));
        TemplateList outputGallery;

        // Set up progress counting variables
        Globals->currentStep = 0;
        Globals->totalSteps = rowSize;
        Globals->startTime.start();

        // Do the actual comparisons
        streamWrapper->projectUpdate(rowGalleryTemplate, outputGallery);
    }

private:
    QString name;

    QString getFileName(const QString &description) const
    {
        const QString file = Globals->sdkPath + "/share/openbr/models/algorithms/" + description;
        return QFileInfo(file).exists() ? file : QString();
    }

    void init(const File &description)
    {
        // Check if a trained binary already exists for this algorithm
        const QString file = getFileName(description);
        if (!file.isEmpty()) return init(file);

        if (description.exists()) {
            qDebug("Loading %s", qPrintable(description.fileName()));
            load(description);
            return;
        }

        // Expand abbreviated algorithms to their full strings
        if (Globals->abbreviations.contains(description))
            return init(Globals->abbreviations[description]);

        //! [Parsing the algorithm description]
        QStringList words = QtUtils::parse(description.flat(), ':');
        if ((words.size() < 1) || (words.size() > 2)) qFatal("Invalid algorithm format.");
        //! [Parsing the algorithm description]

        transformString = words[0];


        //! [Creating the template generation and comparison methods]
        transform = QSharedPointer<Transform>(Transform::make(words[0], NULL));
        if (words.size() > 1) {
            distance = QSharedPointer<Distance>(Distance::make(words[1], NULL));
            distanceString = words[1];
        }
        //! [Creating the template generation and comparison methods]
    }
};

} // namespace br

using namespace br;

class AlgorithmManager : public Initializer
{
    Q_OBJECT

public:
    static QHash<QString, QSharedPointer<AlgorithmCore> > algorithms;
    static QMutex algorithmsLock;

    void initialize() const {}

    void finalize() const
    {
        algorithms.clear();
    }

    static QSharedPointer<AlgorithmCore> getAlgorithm(const QString &algorithm)
    {
        if (algorithm.isEmpty()) qFatal("No default algorithm set.");

        if (!algorithms.contains(algorithm)) {
            // Some algorithms are recursive, so we need to construct them outside the lock.
            QSharedPointer<AlgorithmCore> algorithmCore(new AlgorithmCore(algorithm));

            algorithmsLock.lock();
            if (!algorithms.contains(algorithm))
                algorithms.insert(algorithm, algorithmCore);
            algorithmsLock.unlock();
        }

        return algorithms[algorithm];
    }
};

QHash<QString, QSharedPointer<AlgorithmCore> > AlgorithmManager::algorithms;
QMutex AlgorithmManager::algorithmsLock;

BR_REGISTER(Initializer, AlgorithmManager)

bool br::IsClassifier(const QString &algorithm)
{
    qDebug("Checking if %s is a classifier", qPrintable(algorithm));
    return AlgorithmManager::getAlgorithm(algorithm)->isClassifier();
}

void br::Train(const File &input, const File &model)
{
    AlgorithmManager::getAlgorithm(model.get<QString>("algorithm"))->train(input, model);
}

FileList br::Enroll(const File &input, const File &gallery)
{
    return AlgorithmManager::getAlgorithm(gallery.get<QString>("algorithm"))->enroll(input, gallery);
}

void br::Enroll(TemplateList &tl)
{
    QString alg = tl.first().file.get<QString>("algorithm");
    AlgorithmManager::getAlgorithm(alg)->enroll(tl);
}

void br::Compare(const File &targetGallery, const File &queryGallery, const File &output)
{
    AlgorithmManager::getAlgorithm(output.get<QString>("algorithm"))->compare(targetGallery, queryGallery, output);
}

void br::CompareTemplateLists(const TemplateList &target, const TemplateList &query, Output *output)
{
    QString alg = output->file.get<QString>("algorithm");
    QSharedPointer<Distance> dist = Distance::fromAlgorithm(alg);
    dist->compare(target, query, output);
}

void br::PairwiseCompare(const File &targetGallery, const File &queryGallery, const File &output)
{
    AlgorithmManager::getAlgorithm(output.get<QString>("algorithm"))->pairwiseCompare(targetGallery, queryGallery, output);
}

void br::Convert(const File &fileType, const File &inputFile, const File &outputFile)
{
    qDebug("Converting %s %s to %s", qPrintable(fileType.flat()), qPrintable(inputFile.flat()), qPrintable(outputFile.flat()));

    if (fileType == "Format") {
        QScopedPointer<Format> before(Factory<Format>::make(inputFile));
        QScopedPointer<Format> after(Factory<Format>::make(outputFile));
        after->write(before->read());
    } else if (fileType == "Gallery") {
        QScopedPointer<Gallery> before(Gallery::make(inputFile));
        QScopedPointer<Gallery> after(Gallery::make(outputFile));
        bool done = false;
        while (!done) after->writeBlock(before->readBlock(&done));
    } else if (fileType == "Output") {
        QString target, query;
        cv::Mat m = BEE::readMat(inputFile, &target, &query);
        const FileList targetFiles = TemplateList::fromGallery(target).files();
        const FileList queryFiles = TemplateList::fromGallery(query).files();

        if ((targetFiles.size() != m.cols || queryFiles.size() != m.rows)
            && (m.cols != 1 || targetFiles.size() != m.rows || queryFiles.size() != m.rows))
            qFatal("Similarity matrix (%d, %d) and header (%d, %d) size mismatch.", m.rows, m.cols, queryFiles.size(), targetFiles.size());

        QSharedPointer<Output> o(Factory<Output>::make(outputFile));
        o->initialize(targetFiles, queryFiles);

        if (targetFiles.size() != m.cols)
        {
            MatrixOutput   * mOut = dynamic_cast<MatrixOutput *>(o.data());
            if (mOut)
                mOut->data.create(queryFiles.size(), 1, CV_32FC1);
        }

        o->setBlock(0,0);
        for (int i=0; i < m.rows; i++)
            for (int j=0; j < m.cols; j++)
                o->setRelative(m.at<float>(i,j), i, j);
    } else {
        qFatal("Unrecognized file type %s.", qPrintable(fileType.flat()));
    }
}

void br::Cat(const QStringList &inputGalleries, const QString &outputGallery)
{
    qDebug("Concatenating %d galleries to %s", inputGalleries.size(), qPrintable(outputGallery));
    foreach (const QString &inputGallery, inputGalleries)
        if (inputGallery == outputGallery)
            qFatal("outputGallery must not be in inputGalleries.");
    QScopedPointer<Gallery> og(Gallery::make(outputGallery));
    foreach (const QString &inputGallery, inputGalleries) {
        QScopedPointer<Gallery> ig(Gallery::make(inputGallery));
        bool done = false;
        while (!done) og->writeBlock(ig->readBlock(&done));
    }
}

void br::Deduplicate(const File &inputGallery, const File &outputGallery, const QString &threshold)
{
    bool ok;
    float thresh = threshold.toFloat(&ok);
    if (ok) AlgorithmManager::getAlgorithm(inputGallery.get<QString>("algorithm"))->deduplicate(inputGallery, outputGallery, thresh);
    else qFatal("Unable to convert deduplication threshold to float.");
}

QSharedPointer<br::Transform> br::Transform::fromAlgorithm(const QString &algorithm, bool preprocess)
{
    if (!preprocess)
        return AlgorithmManager::getAlgorithm(algorithm)->transform;
    else {
        QSharedPointer<Transform> orig_tform = AlgorithmManager::getAlgorithm(algorithm)->transform;
        QSharedPointer<Transform> newRoot = QSharedPointer<Transform>(Transform::make("Stream(Identity, readMode=DistributeFrames)", NULL));
        WrapperTransform * downcast = dynamic_cast<WrapperTransform *> (newRoot.data());
        downcast->transform = orig_tform.data();
        downcast->init();
        return newRoot;
    }
}

QSharedPointer<br::Distance> br::Distance::fromAlgorithm(const QString &algorithm)
{
    return AlgorithmManager::getAlgorithm(algorithm)->distance;
}


#include "core.moc"
