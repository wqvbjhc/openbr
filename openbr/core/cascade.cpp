#include "cascade.h"
#include <stdio.h>
#include <iostream>
#include <fstream>

using namespace br;
using namespace cv;

bool CascadeImageReader::create( const QList<Mat> &_posImages, const QList<Mat> &_negImages, Size _winSize )
{
    posImages = _posImages;
    negImages = _negImages;
    winSize = _winSize;

    posIdx = 0; negIdx = 0;
    round = 0;

    src.create( 0, 0 , CV_8UC1 );
    img.create( 0, 0, CV_8UC1 );
    point = offset = Point( 0, 0 );
    scale       = 1.0F;
    scaleFactor = 1.4142135623730950488016887242097F;
    stepFactor  = 0.5F;

    return true;
}

bool CascadeImageReader::nextNeg()
{
    src = negImages[negIdx++];

    round += negIdx / negImages.size();
    round = round % (winSize.width * winSize.height);
    negIdx %= negImages.size();

    offset.x = std::min( (int)round % winSize.width, src.cols - winSize.width );
    offset.y = std::min( (int)round / winSize.width, src.rows - winSize.height );

    point = offset;
    scale = max( ((float)winSize.width + point.x) / ((float)src.cols),
                 ((float)winSize.height + point.y) / ((float)src.rows) );

    Size sz( (int)(scale*src.cols + 0.5F), (int)(scale*src.rows + 0.5F) );
    resize( src, img, sz );
    return true;
}

bool CascadeImageReader::getNeg( Mat& _img )
{
    CV_Assert( !_img.empty() );
    CV_Assert( _img.type() == CV_8UC1 );
    CV_Assert( _img.cols == winSize.width );
    CV_Assert( _img.rows == winSize.height );

    if( img.empty() )
        if ( !nextNeg() )
            return false;

    Mat mat( winSize.height, winSize.width, CV_8UC1, (void*)(img.data + point.y * img.step + point.x * img.elemSize()), img.step );
    mat.copyTo(_img);

    if( (int)( point.x + (1.0F + stepFactor ) * winSize.width ) < img.cols )
        point.x += (int)(stepFactor * winSize.width);
    else
    {
        point.x = offset.x;
        if( (int)( point.y + (1.0F + stepFactor ) * winSize.height ) < img.rows )
            point.y += (int)(stepFactor * winSize.height);
        else
        {
            point.y = offset.y;
            scale *= scaleFactor;
            if( scale <= 1.0F )
                resize( src, img, Size( (int)(scale*src.cols), (int)(scale*src.rows) ) );
            else
            {
                if ( !nextNeg() )
                    return false;
            }
        }
    }
    return true;
}

bool CascadeImageReader::getPos(Mat &_img)
{
    if (posIdx > (int)posImages.size())
        CV_Error( CV_StsBadArg, "Can not get new positive sample. Not enough positive samples.\n");
    _img = posImages[posIdx++];
    return true;
}

//---------------------------- CascadeClassifier --------------------------------------

bool BrCascadeClassifier::train(const string _cascadeDirName,
                                const QList<Mat> &_posImages,
                                const QList<Mat> &_negImages,
                                int _precalcValBufSize, int _precalcIdxBufSize,
                                int _numPos, int _numNeg, int _numStages,
                                Representation *_representation,
                                const CascadeBoostParams& _stageParams)
{
    // Start recording clock ticks for training time output
    const clock_t begin_time = clock();

    if( _cascadeDirName.empty() )
        CV_Error( CV_StsBadArg, "_cascadeDirName is NULL" );

    string dirName;
    if (_cascadeDirName.find_last_of("/\\") == (_cascadeDirName.length() - 1) )
        dirName = _cascadeDirName;
    else
        dirName = _cascadeDirName + '/';

    compat = new BoostBRCompatibility(_representation, _numPos + _numNeg);
    numPos = _numPos;
    numNeg = _numNeg;
    numStages = _numStages;
    imgReader.create(_posImages, _negImages, _representation->windowSize());

    stageParams = new CascadeBoostParams;
    *stageParams = _stageParams;
    stageClassifiers.reserve( numStages );

    double requiredLeafFARate = pow( (double) stageParams->maxFalseAlarm, (double) numStages ) /
                                (double)stageParams->max_depth;
    double tempLeafFARate;

    for (int i = 0; i < numStages; i++) {
        qDebug() << endl << "===== TRAINING " << i << "-stage =====";
        qDebug() << "<BEGIN";
        if ( !updateTrainingSet( tempLeafFARate ) )
        {
            qDebug() << "Train dataset for temp stage can not be filled. "
                "Branch training terminated.";
            break;
        }
        if( tempLeafFARate <= requiredLeafFARate )
        {
            qDebug() << "Required leaf false alarm rate achieved. "
                 "Branch training terminated.";
            break;
        }

        CascadeBoost* tempStage = new CascadeBoost;
        bool isStageTrained = tempStage->train( (BoostBRCompatibility*)compat,
                                                curNumSamples, _precalcValBufSize, _precalcIdxBufSize,
                                                *((CascadeBoostParams*)stageParams) );
        qDebug() << "END>";

        if(!isStageTrained)
            break;

        stageClassifiers.push_back( tempStage );

        // Output training time up till now
        float seconds = float( clock () - begin_time ) / CLOCKS_PER_SEC;
        int days = int(seconds) / 60 / 60 / 24;
        int hours = (int(seconds) / 60 / 60) % 24;
        int minutes = (int(seconds) / 60) % 60;
        int seconds_left = int(seconds) % 60;
        qDebug() << "Training until now has taken " << days << " days " << hours << " hours " << minutes << " minutes " << seconds_left <<" seconds.";
    }

    if(stageClassifiers.size() == 0)
    {
        qDebug() << "Cascade classifier can't be trained. Check the used training parameters.";
        return false;
    }

    save(dirName + CC_CASCADE_FILENAME);

    return true;
}

int BrCascadeClassifier::predict( int sampleIdx )
{
    CV_DbgAssert( sampleIdx < numPos + numNeg );
    for (vector< Ptr<CascadeBoost> >::iterator it = stageClassifiers.begin();
        it != stageClassifiers.end(); it++ )
    {
        if ( (*it)->predict( sampleIdx ) == 0.f )
            return 0;
    }
    return 1;
}

bool BrCascadeClassifier::updateTrainingSet( double& acceptanceRatio)
{
    int64 posConsumed = 0, negConsumed = 0;
    imgReader.restart();

    int posCount = fillPassedSamples( 0, numPos, true, posConsumed );
    if( !posCount )
        return false;
    qDebug() << "POS count : consumed   " << posCount << " : " << (int)posConsumed;

    int proNumNeg = cvRound( ( ((double)numNeg) * ((double)posCount) ) / numPos ); // apply only a fraction of negative samples. double is required since overflow is possible
    int negCount = fillPassedSamples( posCount, proNumNeg, false, negConsumed );
    if ( !negCount )
        return false;

    curNumSamples = posCount + negCount;
    acceptanceRatio = negConsumed == 0 ? 0 : ( (double)negCount/(double)(int64)negConsumed );
    qDebug() << "NEG count : acceptanceRatio    " << negCount << " : " << acceptanceRatio;
    return true;
}

int BrCascadeClassifier::fillPassedSamples( int first, int count, bool isPositive, int64& consumed )
{
    int getcount = 0;
    Mat img(compat->representation->windowSize(), CV_8UC1);
    for( int i = first; i < first + count; i++ )
    {
        for( ; ; )
        {
            bool isGetImg = isPositive ? imgReader.getPos( img ) :
                                         imgReader.getNeg( img );
            if( !isGetImg )
                return getcount;
            consumed++;

            compat->setImage( img, isPositive ? 1 : 0, i );
            if( predict( i ) == 1.0F )
            {
                getcount++;
                printf("%s current samples: %d\r", isPositive ? "POS":"NEG", getcount);
                break;
            }
        }
    }
    return getcount;
}

void BrCascadeClassifier::writeParams( FileStorage &fs ) const
{
    fs << CC_STAGE_TYPE << CC_BOOST;
    fs << CC_FEATURE_TYPE << CC_LBP;
    fs << CC_HEIGHT << winSize.height;
    fs << CC_WIDTH << winSize.width;
    fs << CC_STAGE_PARAMS << "{"; stageParams->write( fs ); fs << "}";
    fs << CC_FEATURE_PARAMS << "{"; fs << CC_MAX_CAT_COUNT << 256; fs << CC_FEATURE_SIZE << 1; fs << "}";
}

void BrCascadeClassifier::writeFeatures( FileStorage &fs, const Mat& featureMap ) const
{
    compat->representation->write(fs, featureMap);
}

void BrCascadeClassifier::writeStages( FileStorage &fs, const Mat& featureMap ) const
{
    char cmnt[30];
    int i = 0;
    fs << CC_STAGES << "[";
    for( vector< Ptr<CascadeBoost> >::const_iterator it = stageClassifiers.begin();
        it != stageClassifiers.end(); it++, i++ )
    {
        sprintf( cmnt, "stage %d", i );
        cvWriteComment( fs.fs, cmnt, 0 );
        fs << "{";
        ((CascadeBoost*)((Ptr<CascadeBoost>)*it))->write( fs, featureMap );
        fs << "}";
    }
    fs << "]";
}

void BrCascadeClassifier::save(const string filename)
{
    FileStorage fs( filename, FileStorage::WRITE );

    if ( !fs.isOpened() )
        return;

    fs << FileStorage::getDefaultObjectName(filename) << "{";

    Mat featureMap;
    getUsedFeaturesIdxMap( featureMap );
    writeParams( fs );
    fs << CC_STAGE_NUM << (int)stageClassifiers.size();
    writeStages( fs, featureMap );
    writeFeatures( fs, featureMap );

    fs << "}";
}

void BrCascadeClassifier::getUsedFeaturesIdxMap( Mat& featureMap )
{
    int varCount = compat->numFeatures();
    featureMap.create( 1, varCount, CV_32SC1 );
    featureMap.setTo(Scalar(-1));

    for( vector< Ptr<CascadeBoost> >::const_iterator it = stageClassifiers.begin();
        it != stageClassifiers.end(); it++ )
        ((CascadeBoost*)((Ptr<CascadeBoost>)(*it)))->markUsedFeaturesInMap( featureMap );

    for( int fi = 0, idx = 0; fi < varCount; fi++ )
        if ( featureMap.at<int>(0, fi) >= 0 )
            featureMap.ptr<int>(0)[fi] = idx++;
}


