/*
Copyright (C) 2019 Mischa Schirmer

This file is part of THELI.

THELI is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program in the LICENSE file.
If not, see https://www.gnu.org/licenses/ .
*/

#include <cmath>
#include "tools.h"
#include "../myimage/myimage.h"
#include <QDebug>

// For debayering
float hue_transit(float l1, float l2, float l3, float v1, float v3)
{
    //printf("hue_transit: l1 %5.1f l2 %5.1f l3 %5.1f v1 %5.1f v3 %5.1f\n",l1,l2,l3,v1,v3);
    if ((l1<l2 && l2<l3) || (l1> l2 && l2 > l3))
        return (v1+(v3-v1) * (l2-l1)/(l3-l1));
    else
        return ((v1+v3)/2.0 + (l2*2.0-l1-l3)/4.0);
}

// For debayering
int direction(float N, float E, float W, float S)
{
    if (N < E && W < S) {
        if ( N < W) return 1;
        else return 3;
    }
    else {
        if (E < S) return 2;
        else return 4;
    }
}

// Qt5 implementation of the PPG (patterened pixel grouping) algorithm by Chuan-Kai Lin,
// originally published at https://sites.google.com/site/chklin/demosaic. The URL is no longer available
// but can be found here:
// https://web.archive.org/web/20160923211135/https://sites.google.com/site/chklin/demosaic/
// First implementation for THELI v2 by Carsten Moos.

// The input image becomes the R-band channel
void debayer(int chip, MyImage *image, MyImage *imageB, MyImage *imageG, MyImage *imageR)
{
    if (!image->successProcessing) return;

    QString pattern = image->getKeyword("BAYER");
    if ( pattern != "RGGB"
         && pattern != "GRBG"
         && pattern != "GBRG"
         && pattern != "BGGR") {
        qDebug() << "Tools::debayer(): Bayer pattern not recognised. Nothing will be done.\n";
        image->successProcessing = false;
        return;
    }

    // chop the last row / column of pixels if the image dimensions are uneven
    int n = image->naxis1;
    int m = image->naxis2;
    if ( n % 2 != 0) n = n - 1;
    if ( m % 2 != 0) m = m - 1;

    // Setup the debayered channels
    QList<MyImage*> list;
    list << imageB << imageG << imageR;
    double mjdOffset = 0.;
    for (auto &it: list) {
        it->naxis1 = n;
        it->naxis2 = m;
        it->dataCurrent.resize(n*m);
        it->dataCurrent.squeeze();
        it->path = image->path;
        it->weightPath = image->weightPath;
        it->baseName = image->rootName;
        it->rootName = image->rootName;
        it->chipName = image->rootName+"_"+QString::number(image->chipNumber);
        it->exptime = image->exptime;
        it->header = image->header;
        it->pathBackupL1 = image->pathBackupL1;
        it->baseNameBackupL1 = image->baseNameBackupL1;
        it->imageInMemory = true;
        it->wcs = image->wcs;
        it->plateScale = image->plateScale;
        it->wcsInit = image->wcsInit;
        it->gain = image->gain;
        it->airmass = image->airmass;
        it->fwhm = image->fwhm;
        it->fwhm_est = image->fwhm_est;
        it->gain = image->gain;
        it->ellipticity = image->ellipticity;
        it->ellipticity_est = image->ellipticity_est;
        it->RZP = image->RZP;
        it->gainNormalization = image->gainNormalization;
        it->hasMJDread = image->hasMJDread;
        it->headerInfoProvided = image->headerInfoProvided;
        it->skyValue = image->skyValue;
        it->modeDetermined = image->modeDetermined;
        it->fullheader = image->fullheader;
        it->dateobs = image->dateobs;
        it->cornersToRaDec();

        // Data::populateExposureList() will group debayered images into one exposure, and then
        // mergeScampCatalogs() will create three extensions. But we need them individually.
        // Hence introducing a 0.1 s offset in MJD-OBS.
        // Fixing Data::populateExposureList() is non-trivial
        it->mjdobs = image->mjdobs + mjdOffset;
        mjdOffset += 1.e-6;
    }
    imageB->baseName.append("_B_"+QString::number(chip+1));  // status 'PA' will be appended externally
    imageG->baseName.append("_G_"+QString::number(chip+1));
    imageR->baseName.append("_R_"+QString::number(chip+1));
    imageB->rootName.append("_B");
    imageG->rootName.append("_G");
    imageR->rootName.append("_R");
    imageB->chipName = imageB->baseName;
    imageG->chipName = imageG->baseName;
    imageR->chipName = imageR->baseName;
    imageB->filter = "B";
    imageG->filter = "G";
    imageR->filter = "R";
    for (auto &card : imageB->header) {
        if (card.contains("FILTER  = ")) {
            card = "FILTER  = 'B'";
            card.resize(80, ' ');
        }
    }
    for (auto &card : imageG->header) {
        if (card.contains("FILTER  = ")) {
            card = "FILTER  = 'G'";
            card.resize(80, ' ');
        }
    }
    for (auto &card : imageR->header) {
        if (card.contains("FILTER  = ")) {
            card = "FILTER  = 'R'";
            card.resize(80, ' ');
        }
    }

    // ==== BEGIN PPG algorithm ===

    // cut all patterns to RGGB
    int xoffset;
    int yoffset;
    if (pattern == "RGGB") {
        xoffset = 0;
        yoffset = 0;
    }
    else if (pattern == "GRBG") {
        xoffset = 1;
        yoffset = 0;
    }
    else if (pattern == "GBRG") {
        xoffset = 0;
        yoffset = 1;
    }
    else {
        // pattern == "BGGR"
        xoffset = 1;
        yoffset = 1;
    }

    /* // The Bayer pattern looks like this
      RGRGRGRGR
      gBgBgBgBg
      RGRGRGRGR
      gBgBgBgBg
    */

    long k = 0;

    QVector<float> &I = image->dataCurrent;
    QVector<float> &R = imageR->dataCurrent;
    QVector<float> &G = imageG->dataCurrent;
    QVector<float> &B = imageB->dataCurrent;

    // Calculate the green values at red and blue pixels
    for (long j=0; j<m; ++j) {
        for (long i=0; i<n; ++i) {

            if( j<=2 || j>=m-3 || i<=2 || i>=n-3) { // 3 rows top bottom left right
                R[k] = I[(i+n*j)];  // all the same with current color
                G[k] = I[(i+n*j)];
                B[k] = I[(i+n*j)];
                ++k;
            }
            else{
                // gradient calculation for green values at red or blue pixels
                float DN = fabs(I[(i-2*n+n*j)]-I[(i+n*j)])*2.0     + fabs(I[(i-n+n*j)]-I[(i+n+n*j)]);
                float DE = fabs(I[(i+n*j)]    -I[(i+2+n*j)])*2.0   + fabs(I[(i-1+n*j)]-I[(i+1+n*j)]);
                float DW = fabs(I[(i-2+n*j)]  -I[(i+n*j)])*2.0     + fabs(I[(i-1+n*j)]-I[(i+1+n*j)]);
                float DS = fabs(I[(i+n*j)]    -I[(i+2*n+n*j)])*2.0 + fabs(I[(i-n+n*j)]-I[(i+n+n*j)]);

                if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0) { // red pixel
                    R[k] = I[(i+n*j)];
                    switch (direction(DN,DE,DW,DS)) {
                    case 1: G[k] = (I[(i-n+n*j)]*3.0 + I[(i+n*j)] + I[(i+n+n*j)] - I[(i-2*n+n*j)]) / 4.0; break;
                    case 2: G[k] = (I[(i+1+n*j)]*3.0 + I[(i+n*j)] + I[(i-1+n*j)] - I[(i+2+n*j)]) / 4.0; break;
                    case 3: G[k] = (I[(i-1+n*j)]*3.0 + I[(i+n*j)] + I[(i+1+n*j)] - I[(i-2+n*j)]) / 4.0; break;
                    case 4: G[k] = (I[(i+n+n*j)]*3.0 + I[(i+n*j)] + I[(i-n+n*j)] - I[(i+2*n+n*j)]) / 4.0; break;
                    }
                    ++k;
                }
                else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1){ // blue pixel
                    switch (direction(DN,DE,DW,DS)){
                    case 1: G[k] = (I[(i-n+n*j)]*3.0 + I[(i+n*j)] + I[(i+n+n*j)] - I[(i-2*n+n*j)]) / 4.0; break;
                    case 2: G[k] = (I[(i+1+n*j)]*3.0 + I[(i+n*j)] + I[(i-1+n*j)] - I[(i+2+n*j)]) / 4.0; break;
                    case 3: G[k] = (I[(i-1+n*j)]*3.0 + I[(i+n*j)] + I[(i+1+n*j)] - I[(i-2+n*j)]) / 4.0; break;
                    case 4: G[k] = (I[(i+n+n*j)]*3.0 + I[(i+n*j)] + I[(i-n+n*j)] - I[(i+2*n+n*j)]) / 4.0; break;
                    }
                    B[k] = I[(i+n*j)];
                    ++k;
                }
                else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0){ // green pixel, red above
                    G[k] = I[(i+n*j)];
                    ++k;
                }
                else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1){ // green pixel, blue above
                    G[k] = I[(i+n*j)];
                    ++k;
                }
            }
        }
    }

    k = 0;
    // Calculating blue and red at green pixels
    // Calculating blue or red at red or blue pixels
    for (long j=0; j<m; ++j) {
        for (long i=0; i<n; ++i) {

            if (j<=2 || j>=m-3 || i<=2 || i>=n-3) {  // 3 rows top bottom left right
                if (!xoffset && !yoffset) {  // RGGB
                    if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0){ // red pixel
                        R[k] = I[(i+n*j)];
                        G[k] = I[(i+1+n*j)];
                        B[k] = I[(i+1+n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0){ // green pixel, red above
                        R[k] = I[(i-n+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i+1+n*j)];
                    }
                    else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1){ // green pixel, blue above
                        R[k] = I[(i-1+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i+n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1){ // blue pixel
                        R[k] = I[(i-1-n+n*j)];
                        G[k] = I[(i-1+n*j)];
                        B[k] = I[(i+n*j)];
                    }
                }
                else if (xoffset && !yoffset) { //GRBG
                    if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0){ // red pixel
                        R[k] = I[(i+n*j)];
                        G[k] = I[(i-1+n*j)];
                        B[k] = I[(i-1+n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0){ // green pixel, red above
                        R[k] = I[(i-n+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i-1+n*j)];
                    }
                    else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1){ // green pixel, blue above
                        R[k] = I[(i+1+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i+n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1){ // blue pixel
                        R[k] = I[(i+1-n+n*j)];
                        G[k] = I[(i+1+n*j)];
                        B[k] = I[(i+n*j)];
                    }
                }
                else if (!xoffset && yoffset) { //GBRG
                    if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0){ // red pixel
                        R[k] = I[(i+n*j)];
                        G[k] = I[(i-n+n*j)];
                        B[k] = I[(i+1-n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0){ // green pixel, red above
                        R[k] = I[(i+n+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i+1+n*j)];
                    }
                    else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1){ // green pixel, blue above
                        R[k] = I[(i-1+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i-n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1){ // blue pixel
                        R[k] = I[(i-1+n+n*j)];
                        G[k] = I[(i-1+n*j)];
                        B[k] = I[(i+n*j)];
                    }
                }
                else if (xoffset && yoffset) { //BGGR
                    if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0){ // red pixel
                        R[k] = I[(i+n*j)];
                        G[k] = I[(i-1+n*j)];
                        B[k] = I[(i-1-n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0){ // green pixel, red above
                        R[k] = I[(i+n+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i-1+n*j)];
                    }
                    else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1){ // green pixel, blue above
                        R[k] = I[(i+1+n*j)];
                        G[k] = I[(i+n*j)];
                        B[k] = I[(i-n+n*j)];
                    }
                    else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1){ // blue pixel
                        R[k] = I[(i+1+n+n*j)];
                        G[k] = I[(i+1+n*j)];
                        B[k] = I[(i+n*j)];
                    }
                }
                ++k;
            }
            else{
                // diagonal gradients
                float dne = fabs(I[(i-n+1+n*j)]-I[(i+n-1+n*j)])
                        + fabs(I[(i-2*n+2+n*j)]-I[(i+n*j)])
                        + fabs(I[(i+n*j)]-I[(i+2*n-2+n*j)])
                        + fabs(G[(i-n+1+n*j)]-G[(i+n*j)])
                        + fabs(G[(i+n*j)]-G[(i+n-1+n*j)]);
                float dnw = fabs(I[(i-n-1+n*j)]-I[(i+n+1+n*j)])
                        + fabs(I[(i-2-2*n+n*j)]-I[(i+n*j)])
                        + fabs(I[(i+n*j)]-I[(i+2+2*n+n*j)])
                        + fabs(G[(i-n-1+n*j)]-G[(i+n*j)])
                        + fabs(G[(i+n*j)]-G[(i+n+1+n*j)]);

                if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 0) { // red pixel
                    if (dne <= dnw)
                        B[k] = hue_transit(G[i-n+1+n*j], G[i+n*j], G[i+n-1+n*j], I[(i-n+1+n*j)], I[(i+n-1+n*j)]);
                    else
                        B[k] = hue_transit(G[i-n-1+n*j], G[i+n*j], G[i+n+1+n*j], I[(i-n-1+n*j)], I[(i+n+1+n*j)]);
                    ++k;
                }
                else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 0) { // green pixel, red above
                    R[k] = hue_transit(G[(i-n+n*j)], I[(i+n*j)], G[(i+n+n*j)], I[(i-n+n*j)], I[(i+n+n*j)]);
                    B[k] = hue_transit(G[(i-1+n*j)], I[(i+n*j)], G[(i+1+n*j)], I[(i-1+n*j)], I[(i+1+n*j)]);
                    ++k;
                }
                else if ((j+yoffset)%2 == 0 && (i+xoffset)%2 == 1) { // green pixel, blue above
                    R[k] = hue_transit(G[(i-1+n*j)], I[(i+n*j)], G[(i+1+n*j)], I[(i-1+n*j)], I[(i+1+n*j)]);
                    B[k] = hue_transit(G[(i-n+n*j)], I[(i+n*j)], G[(i+n+n*j)], I[(i-n+n*j)], I[(i+n+n*j)]);
                    ++k;
                }
                else if ((j+yoffset)%2 == 1 && (i+xoffset)%2 == 1) { // blue pixel
                    if (dne <= dnw)
                        R[k] = hue_transit(G[(i-n+1+n*j)], G[(i+n*j)], G[(i+n-1+n*j)], I[(i-n+1+n*j)], I[(i+n-1+n*j)]);
                    else
                        R[k] = hue_transit(G[(i-n-1+n*j)], G[(i+n*j)], G[(i+n+1+n*j)], I[(i-n-1+n*j)], I[(i+n+1+n*j)]);
                    ++k;
                }
            }
        }
    }

    // === END PPG algorithm ===
}

void updateDebayerMemoryStatus(MyImage *image)
{
    if (!image->successProcessing) return;

    image->imageInMemory = true;
    image->dataBackupL1 = image->dataCurrent;
    image->backupL1InMemory = true;
}

// Remove the relative sensitivity pattern from the bayerflat
// (Calculate an average 2x2 superpixel and divide the flat by it)
void equalizeBayerFlat(MyImage *image)
{
    int n = image->naxis1;
    int m = image->naxis2;

    // calculate the average 2x2 pixel
    float ll = 0.;
    float lr = 0.;
    float ul = 0.;
    float ur = 0.;

    for (int j=0; j<m-1; j+=2) {
        for (int i=0; i<n-1; i+=2) {
            ll += image->dataCurrent[i+n*j];
            lr += image->dataCurrent[i+1+n*j];
            ul += image->dataCurrent[i+n*(j+1)];
            ur += image->dataCurrent[i+1+n*(j+1)];
        }
    }

    ll /= ( n*m / 4. );
    lr /= ( n*m / 4. );
    ul /= ( n*m / 4. );
    ur /= ( n*m / 4. );

    // The four values above have the average intensity of the flat.
    // We want to preserve it after the super pixel normalization
    float sum = (ll + lr + ul + ur) / 4.;
    ll /= sum;
    lr /= sum;
    ul /= sum;
    ur /= sum;

    // Normalize the flat
    for (int j=0; j<m-1; j+=2) {
        for (int i=0; i<n-1; i+=2) {
            image->dataCurrent[i+n*j] /= ll;
            image->dataCurrent[i+1+n*j] /= lr;
            image->dataCurrent[i+n*(j+1)] /= ul;
            image->dataCurrent[i+1+n*(j+1)] /= ur;
        }
    }

    // Update the mode
    image->updateMode();
}
