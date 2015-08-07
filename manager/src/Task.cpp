#include "Task.h"

namespace acc_runtime {

#define LOG_HEADER  std::string("Task::") + \
                    std::string(__func__) +\
                    std::string("(): ")

void Task::addInputBlock(int64_t partition_id, DataBlock_ptr block) {

  input_blocks.push_back(block);

  // add the same block to a map table to provide fast access
  input_table.insert(std::make_pair(partition_id, block));

  // automatically trace all the blocks,
  // if all blocks are initialized with data, 
  // set the task status to READY
  if (block->isReady()) {
    num_ready ++;
    if (num_ready == num_input) {
      status = READY;
    }
  }
}

DataBlock_ptr Task::getInputBlock(int64_t block_id) {
  if (input_table.find(block_id) != input_table.end()) {
    return input_table[block_id];
  }
  else {
    return NULL_DATA_BLOCK;
  }
}

// push one output block to consumer
// return true if there are more blocks to output
bool Task::getOutputBlock(DataBlock_ptr &block) {
  
  if (!output_blocks.empty()) {

    block = output_blocks.back();

    // assuming the blocks are controlled by consumer afterwards
    output_blocks.pop_back();

    // no more output blocks means all data are consumed
    if (output_blocks.empty()) {
      status = COMMITTED;
      return false;
    }
    return true;
  }
  else {
    return false;
  }
}
DataBlock_ptr Task::onDataReady(const DataMsg &blockInfo) {

  int64_t partition_id = blockInfo.partition_id();

  if (input_table.find(partition_id) == input_table.end()) {
    throw std::runtime_error(
        "onDataReady(): Did not find block");
  }

  DataBlock_ptr block = input_table[partition_id];

  // TODO: 
  // multithread issues, two threads can check simutaneously and
  // both see that the block is not ready, and proceed to update
  // the block
  if (!block->isReady()) {

    if (partition_id < 0) {
      if (blockInfo.has_length()) { // if this is a broadcast array

        printf("get data for broadcast data\n");
        // TODO: same as branch length > 0
        int length = blockInfo.length();
        int64_t size = blockInfo.size();
        int num_items = blockInfo.num_items();
        std::string path = blockInfo.path(); 

        // lock block for exclusive access
        boost::lock_guard<DataBlock> guard(*block);

        block->setLength(length);
        block->setNumItems(num_items);

        // allocate memory for block
        block->alloc(size);

        // read block from memory mapped file
        block->readFromMem(path);
      }
      else if (blockInfo.has_bval()) { // if this is a broadcast scalar

        int64_t bval = blockInfo.bval();

        // lock block for exclusive access
        boost::lock_guard<DataBlock> guard(*block);

        block->setLength(1);
        block->setNumItems(1);

        block->alloc(8);

        // add the scalar as a new block
        block->writeData((void*)&bval, 8);
      }
      else {
        throw std::runtime_error(
            "onDataReady(): Invalid broadcast data msg");
      }
    }
    else {

      int length = blockInfo.length();
      int num_items = blockInfo.has_num_items() ? 
        blockInfo.num_items() : 1;
      int64_t size = blockInfo.size();	
      int64_t offset = blockInfo.offset();
      std::string path = blockInfo.path();

      if (length == -1) { // read from file

        //std::vector<std::string> lines;
        char* buffer = new char[size];

        if (path.compare(0, 7, "hdfs://") == 0) {
          // read from HDFS

#ifdef USE_HDFS
          if (!getenv("HDFS_NAMENODE") ||
              !getenv("HDFS_PORT"))
          {
            throw std::runtime_error(
                "no HDFS_NAMENODE or HDFS_PORT defined");
          }

          std::string hdfs_name_node = getenv("HDFS_NAMENODE");
          uint16_t hdfs_port = 
            boost::lexical_cast<uint16_t>(getenv("HDFS_PORT"));

          hdfsFS fs = hdfsConnect(hdfs_name_node.c_str(), hdfs_port);

          if (!fs) {
            throw std::runtime_error("Cannot connect to HDFS");
          }

          hdfsFile fin = hdfsOpenFile(fs, path.c_str(), O_RDONLY, 0, 0, 0); 

          if (!fin) {
            throw std::runtime_error("Cannot find file in HDFS");
          }

          int err = hdfsSeek(fs, fin, offset);
          if (err != 0) {
            throw std::runtime_error(
                "Cannot read HDFS from the specific position");
          }

          int64_t bytes_read = hdfsRead(
              fs, fin, (void*)buffer, size);

          if (bytes_read != size) {
            throw std::runtime_error("HDFS read error");
          }

          hdfsCloseFile(fs, fin);
#else 
          throw std::runtime_error("HDFS file is not supported");
#endif
        }
        else {
          // read from normal file
          std::ifstream fin(path, std::ifstream::binary); 

          if (!fin) {
            throw std::runtime_error("Cannot find file");
          }

          // TODO: error handling
          fin.seekg(offset);
          fin.read(buffer, size);
          fin.close();
        }

        std::string line_buffer(buffer);
        delete buffer;

        // buffer for all data
        std::vector<std::pair<size_t, char*> > data_buf;
        size_t total_bytes = 0;

        // split the file by newline
        std::istringstream sstream(line_buffer);
        std::string line;

        size_t num_bytes = 0;      
        size_t num_elements = 0;      

        while(std::getline(sstream, line)) {

          char* data;
          try {
            data = readLine(line, num_elements, num_bytes);
          } catch (std::runtime_error &e) {
            throw e; 
          }

          if (num_bytes > 0) {
            data_buf.push_back(std::make_pair(num_bytes, data));

            total_bytes += num_bytes;

          }
        }

        if (total_bytes > 0) {

          // lock block for exclusive access
          boost::lock_guard<DataBlock> guard(*block);

          // copy data to block
          block->alloc(total_bytes);

          size_t offset = 0;
          for (int i=0; i<data_buf.size(); i++) {
            size_t bytes = data_buf[i].first;
            char* data   = data_buf[i].second;

            block->writeData((void*)data, bytes, offset);
            offset += bytes;

            delete data;
          }

          // the number of items is equal to the number of lines
          block->setNumItems(data_buf.size());

          // the total data length is num_elements * num_lines
          block->setLength(num_elements*data_buf.size());
        }

      }
      else {  // read from memory mapped file

        // lock block for exclusive access
        boost::lock_guard<DataBlock> guard(*block);

        block->setLength(length);
        block->setNumItems(num_items);

        // allocate memory for block
        block->alloc(size);

        block->readFromMem(path);
      }
    }
  }
  num_ready++;

  if (num_ready == num_input) {
    status = READY;
  }

  return block;
}

} // namespace
