package dev.bedaim.bridge.shizuku
import androidx.annotation.Keep
import dev.bedaim.bridge.IBridgeUserService
import java.io.File
import java.io.RandomAccessFile
import kotlin.system.exitProcess
/** Shizuku shell-identity file service for shared user storage (/sdcard). */
@Keep
class BridgeUserService : IBridgeUserService.Stub() {
 private val root=File("/sdcard").canonicalFile
 override fun listFiles(path:String?):Array<String>{
  val dir=resolve(path); require(dir.isDirectory){"Directory not found: $path"}
  return dir.listFiles().orEmpty().sortedBy{it.name}.map{ "${if(it.isDirectory)"DIR" else "FILE"}\t${it.name}\t${if(it.isFile)it.length() else 0}" }.toTypedArray()
 }
 override fun readFile(path:String,offset:Long,length:Int):ByteArray{
  require(offset>=0 && length in 1..MAX_CHUNK){"Invalid read range; max chunk 256 KiB"}
  val f=resolve(path); require(f.isFile && offset<=f.length()){"File not found or offset out of range"}
  val n=minOf(length.toLong(),f.length()-offset).toInt(); val b=ByteArray(n)
  RandomAccessFile(f,"r").use{it.seek(offset);it.readFully(b)}; return b
 }
 override fun writeFile(path:String,content:ByteArray,offset:Long,truncate:Boolean):String{
  require(offset>=0 && content.size<=MAX_CHUNK){"Invalid write range; max chunk 256 KiB"}
  val f=resolve(path); require(!f.exists()||f.isFile){"Target is not a regular file"}
  f.parentFile?.mkdirs(); RandomAccessFile(f,"rw").use{if(truncate)it.setLength(0);it.seek(offset);it.write(content)}
  return "Wrote ${content.size} bytes at offset $offset: ${f.path}"
 }
 override fun makeDirectory(path:String):String{val f=resolve(path);require(f.mkdirs()||f.isDirectory){"Could not create directory"};return "Directory ready: ${f.path}"}
 override fun deletePath(path:String,recursive:Boolean):String{
  val f=resolve(path);require(f!=root){"Refusing to delete storage root"};require(f.exists()){"Path does not exist"}
  if(f.isDirectory&&f.listFiles()?.isNotEmpty()==true)require(recursive){"Directory not empty; set recursive=true"}
  val ok=if(f.isDirectory&&recursive)f.deleteRecursively() else f.delete();require(ok){"Delete failed"};return "Deleted: ${f.path}"
 }
 override fun copyPath(source:String,destination:String,overwrite:Boolean):String{
  val src=resolve(source);val dst=resolve(destination);require(src.exists()&&src!=root){"Invalid source"};require(!dst.exists()||overwrite){"Destination exists"}
  if(src.isDirectory)src.copyRecursively(dst,overwrite) else {dst.parentFile?.mkdirs();src.copyTo(dst,overwrite)}
  return "Copied ${src.path} -> ${dst.path}"
 }
 override fun movePath(source:String,destination:String,overwrite:Boolean):String{
  val src=resolve(source);val dst=resolve(destination);require(src.exists()&&src!=root){"Invalid source"};require(!dst.exists()||overwrite){"Destination exists"}
  dst.parentFile?.mkdirs();if(dst.exists()&&overwrite){if(dst.isDirectory)dst.deleteRecursively() else dst.delete()}
  if(!src.renameTo(dst)){if(src.isDirectory)src.copyRecursively(dst,overwrite) else src.copyTo(dst,overwrite);if(src.isDirectory)src.deleteRecursively() else src.delete()}
  return "Moved ${src.path} -> ${dst.path}"
 }
 private fun resolve(input:String?):File{
  val raw=input?.takeIf{it.isNotBlank()}?: "/sdcard"
  val candidateInput=when{raw=="/sdcard"||raw=="/storage/emulated/0"->root;raw.startsWith("/sdcard/")->File(root,raw.removePrefix("/sdcard/"));raw.startsWith("/storage/emulated/0/")->File(root,raw.removePrefix("/storage/emulated/0/"));File(raw).isAbsolute->File(raw);else->File(root,raw)}
  val f=candidateInput.canonicalFile;val prefix=root.path.trimEnd(File.separatorChar)+File.separator
  require(f==root||f.path.startsWith(prefix)){"Path outside shared user storage (/sdcard): $input"};return f
 }
 override fun destroy(){exitProcess(0)}
 companion object{private const val MAX_CHUNK=262144}
}