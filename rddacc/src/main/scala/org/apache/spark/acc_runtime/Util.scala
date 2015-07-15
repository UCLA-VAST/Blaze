package org.apache.spark.acc_runtime

import java.io._
import java.util.Calendar
import java.text.SimpleDateFormat
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.channels.FileChannel.MapMode;
import java.io.OutputStream    
import java.io.FileOutputStream

import scala.reflect.{ClassTag, classTag}
import scala.reflect.runtime.universe._         
import scala.collection.mutable        
import scala.collection.mutable.HashMap
                                                
import org.apache.spark._                       
import org.apache.spark.{Partition, TaskContext}
import org.apache.spark.rdd._                   

object Util {

  // These config values should be moved to another sutiable place.
  // Format: | Broadcast, 1 | App ID, 32 | RDD ID, 18 | Partition ID, 12 | Sub-block ID, 1 |
  val APP_BIT_START = 31
  val RDD_BIT_START = 13
  val PARTITION_BIT_START = 1
  val BLOCK_BIT_START = 0

  val logFile = new PrintWriter(new File("acc_runtime.log"))
  println("Logging ACCRuntime at acc_runtime.log")

  def logInfo[T: ClassTag](clazz: T, msg: String) = {
    val clazzName = clazz.getClass.getName.replace("org.apache.spark.acc_runtime.", "")
    var name = clazzName
    if (name.indexOf("$") != -1)
      name = name.substring(0, name.indexOf("$"))

    val time = Calendar.getInstance.getTime
    logFile.write("[INFO][" + time + "] " + name + ": " + msg + "\n")
  }

  def closeLog() = {
    logFile.close
  }

  def getTypeSizeByRDD[T: ClassTag](rdd: RDD[T]): Int = {
    if (classTag[T] == classTag[Byte])        1
    else if (classTag[T] == classTag[Short])  2
    else if (classTag[T] == classTag[Char])   2
    else if (classTag[T] == classTag[Int])    4
    else if (classTag[T] == classTag[Float])  4
    else if (classTag[T] == classTag[Long])   8
    else if (classTag[T] == classTag[Double]) 8
    else 0
  }

  def getTypeSizeByName(dataType: String): Int = {
    val typeSize: Int = dataType match {
      case "int" => 4
      case "integer" => 4
      case "float" => 4
      case "long" => 8
      case "double" => 8
      case _ => 0
    }
    typeSize
  }

  def casting[T: ClassTag, U: ClassTag](value: T, clazz: Class[U]): U = {
 	  val buf = ByteBuffer.allocate(8)
	  buf.order(ByteOrder.LITTLE_ENDIAN)

	  if (value.isInstanceOf[Int])
	    buf.putInt(value.asInstanceOf[Int])
    else if (value.isInstanceOf[Float])
	    buf.putFloat(value.asInstanceOf[Float])
    else if (value.isInstanceOf[Long])
	    buf.putLong(value.asInstanceOf[Long])
    else if (value.isInstanceOf[Double])
	    buf.putDouble(value.asInstanceOf[Double])
    else
      throw new RuntimeException("Unsupported casting type")

    if (clazz == classOf[Long])
  	  buf.getLong(0).asInstanceOf[U]
    else if (clazz == classOf[Int])
      buf.getInt(0).asInstanceOf[U]
    else if (clazz == classOf[Float])
      buf.getFloat(0).asInstanceOf[U]
    else if (clazz == classOf[Double])
      buf.getDouble(0).asInstanceOf[U]
    else
      throw new RuntimeException("Unsupported casting type")
  }

  def getBlockID(first: Int, second: Int, third: Int = -1, fourth: Int = -1): Long = {
    if (third == -1) { // broadcast block
      -((first.toLong << APP_BIT_START) + (second + 1))
    }
    else { // normal block
      (first.toLong << APP_BIT_START) + (second << RDD_BIT_START) + 
        (third << PARTITION_BIT_START) + (fourth << BLOCK_BIT_START)
    }
  }

  def serializePartition[T: ClassTag](prefix: Int, input: Array[T], id: Long): (String, Int) = {
    var fileName: String = System.getProperty("java.io.tmpdir") + "/" + prefix
    if (id < 0) // Broadcast data
      fileName = fileName + "_brdcst_" + (-id) + ".dat"
    else // Normal data
      fileName = fileName + id + ".dat"

    val dataType: String = input(0).getClass.getName.replace("java.lang.", "").toLowerCase()
    val typeSize: Int = getTypeSizeByName(dataType)

    // Create and write memory mapped file
    var raf: RandomAccessFile = null

    try {
      raf = new RandomAccessFile(fileName, "rw")
    } catch {
      case e: IOException =>
        logInfo(this, "Fail to create memory mapped file " + fileName + ": " + e.toString)
    }
    val fc: FileChannel = raf.getChannel()
    val buf: ByteBuffer = fc.map(MapMode.READ_WRITE, 0, input.length * typeSize)
    buf.order(ByteOrder.LITTLE_ENDIAN)

    for (e <- input) {
      dataType match {
        case "int" => buf.putInt(e.asInstanceOf[Int].intValue)
        case "integer" => buf.putInt(e.asInstanceOf[Int].intValue)
        case "float" => buf.putFloat(e.asInstanceOf[Float].floatValue)
        case "long" => buf.putLong(e.asInstanceOf[Long].longValue)
        case "double" => buf.putDouble(e.asInstanceOf[Double].doubleValue)
        case _ =>
          throw new RuntimeException("Unsupported type " + dataType)
      }
    }
   
    try {
      fc.close()
      raf.close()
    } catch {
      case e: IOException =>
        logInfo(this, "Fail to close memory mapped file " + fileName + ": " + e.toString)
    }

    (fileName, input.length)
  }

  def readMemoryMappedFile[T: ClassTag](
      out: Array[T], 
      offset: Int, 
      length: Int,
      fileName: String) = {

     // Fetch size information
    val dataType: String = out(0).getClass.getName.replace("java.lang.", "").toLowerCase()
    val typeSize: Int = getTypeSizeByName(dataType)

    // Create and write memory mapped file
    var raf: RandomAccessFile = null

    try {
      raf = new RandomAccessFile(fileName, "r")
    } catch {
      case e: IOException =>
        logInfo(this, "Fail to read memory mapped file " + fileName + ": " + e.toString)
    }

    val fc: FileChannel = raf.getChannel()
    val buf: ByteBuffer = fc.map(MapMode.READ_ONLY, 0, length * typeSize)
    buf.order(ByteOrder.LITTLE_ENDIAN)

    var idx: Int = offset
    while (idx < offset + length) {
      dataType match {
        case "int" => out(idx) = buf.getInt().asInstanceOf[T]
        case "integer" => out(idx) = buf.getInt().asInstanceOf[T]
        case "float" => out(idx) = buf.getFloat().asInstanceOf[T]
        case "long" => out(idx) = buf.getLong().asInstanceOf[T]
        case "double" => out(idx) = buf.getDouble().asInstanceOf[T]
        case _ =>
          throw new RuntimeException("Unsupported type" + dataType)
      }
      idx = idx + 1
    }
   
    try {
      fc.close()
      raf.close()
    } catch {
      case e: IOException =>
        logInfo(this, "Fail to close memory mapped file " + fileName + ": " + e.toString)
    }
  }
}
