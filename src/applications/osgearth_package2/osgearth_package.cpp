/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2013 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osg/io_utils>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/WriteFile>

#include <osgEarth/Common>
#include <osgEarth/Map>
#include <osgEarth/MapNode>
#include <osgEarth/Registry>
#include <osgEarth/StringUtils>
#include <osgEarth/HTTPClient>
#include <osgEarth/TileVisitor>
#include <osgEarthUtil/TMSPackager>
#include <osgEarthDrivers/feature_ogr/OGRFeatureOptions>
#include <osgEarthDrivers/tms/TMSOptions>

#include <iostream>
#include <sstream>
#include <iterator>

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Drivers;

#define LC "[osgearth_package] "


/** Prints an error message, usage information, and returns -1. */
int
usage( const std::string& msg = "" )
{
    if( !msg.empty() )
    {
        std::cout << msg << std::endl;
    }

    std::cout
        << std::endl
        << "USAGE: osgearth_package <earth_file>" << std::endl
        << std::endl
        << "         --tms                              : make a TMS repo\n"
        << "            <earth_file>                    : earth file defining layers to export (required)\n"
        << "            --out <path>                    : root output folder of the TMS repo (required)\n"
        << "            [--bounds xmin ymin xmax ymax]* : bounds to package (in map coordinates; default=entire map)\n"
        << "            [--max-level <num>]             : max LOD level for tiles (all layers; default=inf)\n"
        << "            [--out-earth <earthfile>]       : export an earth file referencing the new repo\n"
        << "            [--ext <extension>]             : overrides the image file extension (e.g. jpg)\n"
        << "            [--overwrite]                   : overwrite existing tiles\n"
        << "            [--keep-empties]                : writes out fully transparent image tiles (normally discarded)\n"
        << "            [--continue-single-color]       : continues to subdivide single color tiles, subdivision typicall stops on single color images\n"
        << "            [--elevation-pixel-depth]       : pixeldepth for elevations\n"
        << "            [--db-options]                : db options string to pass to the image writer in quotes (e.g., \"JPEG_QUALITY 60\")\n"
        << std::endl
        << "         [--quiet]               : suppress progress output" << std::endl;

    return -1;
}


/** Prints a message and returns a non-error return code. */
int
message( const std::string& msg )
{
    if( !msg.empty() )
    {
        std::cout << msg << std::endl << std::endl;
    }
    return 0;
}


/** Finds an argument with the specified extension. */
std::string
findArgumentWithExtension( osg::ArgumentParser& args, const std::string& ext )
{
    for( int i = 0; i < args.argc(); ++i )
    {
        std::string arg( args.argv()[i] );
        if( endsWith( toLower( trim( arg ) ), ".earth" ) )
            return arg;
    }
    return "";
}


/** Packages an image layer as a TMS folder. */
int
makeTMS( osg::ArgumentParser& args )
{
    osgDB::Registry::instance()->getReaderWriterForExtension("png");
    osgDB::Registry::instance()->getReaderWriterForExtension("jpg");
    osgDB::Registry::instance()->getReaderWriterForExtension("tiff");

    //Read the min level
    unsigned int minLevel = 0;
    while (args.read("--min-level", minLevel));

    //Read the max level
    unsigned int maxLevel = 5;
    while (args.read("--max-level", maxLevel));
    

    std::vector< Bounds > bounds;
    // restrict packaging to user-specified bounds.    
    double xmin=DBL_MAX, ymin=DBL_MAX, xmax=DBL_MIN, ymax=DBL_MIN;
    while (args.read("--bounds", xmin, ymin, xmax, ymax ))
    {        
        Bounds b;
        b.xMin() = xmin, b.yMin() = ymin, b.xMax() = xmax, b.yMax() = ymax;
        bounds.push_back( b );
    }    

    std::string tileList;
    while (args.read( "--tiles", tileList ) );

    bool verbose = args.read("--verbose");

    unsigned int batchSize = 0;
    args.read("--batchsize", batchSize);

    // Read the concurrency level
    unsigned int concurrency = 0;
    args.read("-c", concurrency);
    args.read("--concurrency", concurrency);

    bool writeXML = true;

    // load up the map
    osg::ref_ptr<MapNode> mapNode = MapNode::load( args );
    if( !mapNode.valid() )
        return usage( "Failed to load a valid .earth file" );


    // Read in an index shapefile
    std::string index;
    while (args.read("--index", index))
    {        
        //Open the feature source
        OGRFeatureOptions featureOpt;
        featureOpt.url() = index;        

        osg::ref_ptr< FeatureSource > features = FeatureSourceFactory::create( featureOpt );
        features->initialize();
        features->getFeatureProfile();

        osg::ref_ptr< FeatureCursor > cursor = features->createFeatureCursor();
        while (cursor.valid() && cursor->hasMore())
        {
            osg::ref_ptr< Feature > feature = cursor->nextFeature();
            osgEarth::Bounds featureBounds = feature->getGeometry()->getBounds();
            GeoExtent ext( feature->getSRS(), featureBounds );
            ext = ext.transform( mapNode->getMapSRS() );
            bounds.push_back( ext.bounds() );            
        }
    }

    // see if the user wants to override the type extension (imagery only)
    std::string extension;
    args.read( "--ext", extension );

    // find a .earth file on the command line
    std::string earthFile = findArgumentWithExtension( args, ".earth" );
    
    // folder to which to write the TMS archive.
    std::string rootFolder;
    if( !args.read( "--out", rootFolder ) )
        rootFolder = Stringify() << earthFile << ".tms_repo";

    // whether to overwrite existing tile files
    //TODO:  Support
    bool overwrite = false;
    if( args.read( "--overwrite" ) )
        overwrite = true;

    // write out an earth file
    std::string outEarth;
    args.read( "--out-earth", outEarth );

    std::string dbOptions;
    args.read( "--db-options", dbOptions );
    std::string::size_type n = 0;
    while( (n = dbOptions.find( '"', n )) != dbOptions.npos )
    {
        dbOptions.erase( n, 1 );
    }

    osg::ref_ptr<osgDB::Options> options = new osgDB::Options( dbOptions );

    // whether to keep 'empty' tiles
    //TODO:  Add support
    bool keepEmpties = args.read( "--keep-empties" );

    //TODO:  Single color
    bool continueSingleColor = args.read( "--continue-single-color" );

    // elevation pixel depth
    unsigned elevationPixelDepth = 32;
    args.read( "--elevation-pixel-depth", elevationPixelDepth );
    
    // create a folder for the output
    osgDB::makeDirectory( rootFolder );
    if( !osgDB::fileExists( rootFolder ) )
        return usage( "Failed to create root output folder" );

    int imageLayerIndex = -1;
    args.read("--image", imageLayerIndex);

    int elevationLayerIndex = -1;
    args.read("--elevation", elevationLayerIndex);
    
    Map* map = mapNode->getMap();


    osg::ref_ptr< TileVisitor > visitor;

    // If we are given a task file, load it up and create a new TileKeyListVisitor
    if (!tileList.empty())
    {        
        TaskList tasks( mapNode->getMap()->getProfile() );
        tasks.load( tileList );

        TileKeyListVisitor* v = new TileKeyListVisitor();
        v->setKeys( tasks.getKeys() );
        visitor = v;     
        // This process is a lowly worker, and shouldn't write out the XML file.
        writeXML = false;
    }

    // If we dont' have a visitor create one.
    if (!visitor.valid())
    {
        if (args.read("--mt"))
        {
            // Create a multithreaded visitor
            MultithreadedTileVisitor* v = new MultithreadedTileVisitor();
            if (concurrency > 0)
            {
                v->setNumThreads(concurrency);
            }
            visitor = v;            
        }
        else if (args.read("--mp"))
        {
            // Create a multiprocess visitor
            MultiprocessTileVisitor* v = new MultiprocessTileVisitor();
            if (concurrency > 0)
            {
                v->setNumProcesses(concurrency);
            }

            if (batchSize > 0)
            {            
                v->setBatchSize(batchSize);
            }


            // Try to find the earth file
            std::string earthFile;
            for(int pos=1;pos<args.argc();++pos)
            {
                if (!args.isOption(pos))
                {
                    earthFile  = args[ pos ];
                }
            }
            std::stringstream baseCommand;
            baseCommand << "osgearth_package2 --tms ";
            
            // Serialize the options into the new process
            if (!extension.empty())
            {
                baseCommand << " --extension " << extension << " ";
            }

            if (overwrite)
            {
                baseCommand < " --overwrite ";
            }

            if (!dbOptions.empty())
            {
                baseCommand << " --db-options " << dbOptions << " ";
            }

            if (keepEmpties)
            {
                baseCommand << " --keep-empties " << std::endl;
            }
 
            if (continueSingleColor)
            {
                baseCommand << " --continue-single-color ";
            }

            baseCommand << " --elevation-pixel-depth " << elevationPixelDepth << " ";

            if (imageLayerIndex >= 0)
            {
                baseCommand << " --image " << imageLayerIndex;
            }
            else if (elevationLayerIndex >= 0)
            {
                baseCommand << " --elevation " << elevationLayerIndex;
            }

            baseCommand << " " << earthFile;                     
            v->setBaseCommand(baseCommand.str());
            visitor = v;            
        }
        else
        {
            // Create a single thread visitor
            visitor = new TileVisitor();            
        }        
    }

    osg::ref_ptr< ProgressCallback > progress = new ConsoleProgressCallback();

    if (verbose)
    {
        visitor->setProgressCallback( progress );
    }

    visitor->setMinLevel( minLevel );
    visitor->setMaxLevel( maxLevel );        


    for (unsigned int i = 0; i < bounds.size(); i++)
    {
        GeoExtent extent(mapNode->getMapSRS(), bounds[i]);
        OE_DEBUG << "Adding extent " << extent.toString() << std::endl;                
        visitor->addExtent( extent );
    }    


    // Setup a TMSPackager with all the options.
    TMSPackager packager;
    packager.setVisitor(visitor);
    packager.setDestination(rootFolder);    
    packager.setElevationPixelDepth(elevationPixelDepth);
    packager.setWriteOptions(options);    


    // new map for an output earth file if necessary.
    osg::ref_ptr<Map> outMap = 0L;
    if( !outEarth.empty() )
    {
        // copy the options from the source map first
        outMap = new Map( map->getInitialMapOptions() );
    }

    std::string outEarthFile = osgDB::concatPaths( rootFolder, osgDB::getSimpleFileName( outEarth ) );

    // If we are using a MultiProcessTileVisitor save the base command so we can modify it between layers
    MultiprocessTileVisitor* mp = dynamic_cast<MultiprocessTileVisitor*>(visitor.get());
    std::string baseCommand;
    if (mp)
    {
        baseCommand = mp->getBaseCommand();
    }


    // Package an individual image layer
    if (imageLayerIndex >= 0)
    {        
        ImageLayer* layer = map->getImageLayerAt(imageLayerIndex);
        if (layer)
        {
            packager.run(layer, map->getProfile());
            if (writeXML)
            {
                packager.writeXML(layer, map->getProfile());
            }
        }
        else
        {
            std::cout << "Failed to find an image layer at index " << imageLayerIndex << std::endl;
            return 1;
        }
    }
    // Package an individual elevation layer
    else if (elevationLayerIndex >= 0)
    {
        packager.setExtension("tif");
        ElevationLayer* layer = map->getElevationLayerAt(elevationLayerIndex);
        if (layer)
        {
            packager.run(layer, map->getProfile());
            if (writeXML)
            {
                packager.writeXML(layer, map->getProfile());
            }
        }
        else
        {
            std::cout << "Failed to find an elevation layer at index " << elevationLayerIndex << std::endl;
            return 1;
        }
    }
    else
    {        
        // Package all the ImageLayer's
        for (unsigned int i = 0; i < map->getNumImageLayers(); i++)
        {
            if (mp)
            {                
                std::stringstream buf;
                buf << baseCommand << " --image " << i;                
                mp->setBaseCommand(buf.str());
            }
            

            ImageLayer* layer = map->getImageLayerAt(i);        
            OE_NOTICE << "Packaging " << layer->getName() << std::endl;
            osg::Timer_t start = osg::Timer::instance()->tick();
            packager.run(layer, map->getProfile());
            osg::Timer_t end = osg::Timer::instance()->tick();
            if (verbose)
            {
                OE_NOTICE << "Completed seeding layer " << layer->getName() << " in " << prettyPrintTime( osg::Timer::instance()->delta_s( start, end ) ) << std::endl;
            }                

            if (writeXML)
            {
                packager.writeXML(layer, map->getProfile());
            }

            // save to the output map if requested:
            if( outMap.valid() )
            {
                std::string layerFolder = toLegalFileName( layer->getName() );

                // new TMS driver info:
                TMSOptions tms;
                tms.url() = URI(
                    osgDB::concatPaths( layerFolder, "tms.xml" ),
                    outEarthFile );

                ImageLayerOptions layerOptions( layer->getName(), tms );
                layerOptions.mergeConfig( layer->getInitialOptions().getConfig( true ) );
                layerOptions.cachePolicy() = CachePolicy::NO_CACHE;

                outMap->addImageLayer( new ImageLayer( layerOptions ) );
            }
        }    

        // For elevation layers we need to use tiff
        packager.setExtension("tif");
        // Package all the ElevationLayer's
        for (unsigned int i = 0; i < map->getNumElevationLayers(); i++)
        {
            if (mp)
            {                
                std::stringstream buf;
                buf << baseCommand << " --elevation " << i;                
                mp->setBaseCommand(buf.str());
            }

            ElevationLayer* layer = map->getElevationLayerAt(i);        
            OE_NOTICE << "Packaging " << layer->getName() << std::endl;
            osg::Timer_t start = osg::Timer::instance()->tick();
            packager.run(layer, map->getProfile());
            osg::Timer_t end = osg::Timer::instance()->tick();
            if (verbose)
            {
                OE_NOTICE << "Completed seeding layer " << layer->getName() << " in " << prettyPrintTime( osg::Timer::instance()->delta_s( start, end ) ) << std::endl;
            }      
            if (writeXML)
            {
                packager.writeXML(layer, map->getProfile());
            }

            // save to the output map if requested:
            if( outMap.valid() )
            {
                std::string layerFolder = toLegalFileName( layer->getName());

                // new TMS driver info:
                TMSOptions tms;
                tms.url() = URI(
                    osgDB::concatPaths( layerFolder, "tms.xml" ),
                    outEarthFile );

                ElevationLayerOptions layerOptions( layer->getName(), tms );
                layerOptions.mergeConfig( layer->getInitialOptions().getConfig( true ) );
                layerOptions.cachePolicy() = CachePolicy::NO_CACHE;

                outMap->addElevationLayer( new ElevationLayer( layerOptions ) );
            }
        }

    }

    // Write out an earth file if it was requested
    // Finally, write an earth file if requested:
    if( outMap.valid() )
    {
        MapNodeOptions outNodeOptions = mapNode->getMapNodeOptions();
        osg::ref_ptr<MapNode> outMapNode = new MapNode( outMap.get(), outNodeOptions );
        if( !osgDB::writeNodeFile( *outMapNode.get(), outEarthFile ) )
        {
            OE_WARN << LC << "Error writing earth file to \"" << outEarthFile << "\"" << std::endl;
        }
        else if( verbose )
        {
            OE_NOTICE << LC << "Wrote earth file to \"" << outEarthFile << "\"" << std::endl;
        }
    }

    return 0;
}

/**
 * Data packaging tool for osgEarth.
 */
int
main( int argc, char** argv )
{
    osg::ArgumentParser args( &argc, argv );

    HTTPClient::setUserAgent( "osgearth_package/2.2" );

    if( args.read( "--tms" ) )
        return makeTMS( args );

    else
        return usage();
}