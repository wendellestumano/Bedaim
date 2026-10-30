package dev.bedaim.bridge.ui
import android.Manifest
import android.app.Activity
import android.content.*
import android.content.pm.PackageManager
import android.os.*
import android.view.ViewGroup
import android.widget.*
import androidx.core.content.ContextCompat
import dev.bedaim.bridge.core.*
import dev.bedaim.bridge.security.BridgePreferences
import dev.bedaim.bridge.service.BridgeService
import rikka.shizuku.Shizuku
import java.io.File
class MainActivity:Activity() {
    private lateinit var status:TextView
    override fun onCreate(savedInstanceState:Bundle?){super.onCreate(savedInstanceState);if(Build.VERSION.SDK_INT>=33&&checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)!=PackageManager.PERMISSION_GRANTED)requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS),9);render()}
    override fun onResume(){super.onResume();if(::status.isInitialized)refresh()}
    private fun render(){
        val root=LinearLayout(this).apply{orientation=1;setPadding(32,32,32,32)}
        val scroll=ScrollView(this).apply{addView(root)};setContentView(scroll)
        root.addView(TextView(this).apply{text="Termux MCP Bridge";textSize=26f})
        root.addView(TextView(this).apply{text="Android como estação remota de desenvolvimento BedAim.";textSize=14f})
        status=TextView(this).apply{textSize=15f;setPadding(0,20,0,20)};root.addView(status)
        button(root,"Iniciar servidor MCP"){ContextCompat.startForegroundService(this,Intent(this,BridgeService::class.java).setAction(BridgeService.ACTION_START));status.postDelayed({refresh()},500)}
        button(root,"Parar servidor"){startService(Intent(this,BridgeService::class.java).setAction(BridgeService.ACTION_STOP));status.postDelayed({refresh()},300)}
        root.addView(TextView(this).apply{text="Endpoint local: http://127.0.0.1:${BridgeService.PORT}/mcp\n\nToken MCP (confidencial):\n${BridgePreferences.getOrCreateToken(this@MainActivity)}\n\nO servidor está limitado ao loopback. O túnel remoto ainda será adicionado.";textSize=13f;setPadding(0,20,0,20);setTextIsSelectable(true)})
        button(root,"Solicitar permissão Shizuku"){requestShizuku()}
        button(root,"Abrir Shizuku"){packageManager.getLaunchIntentForPackage("moe.shizuku.privileged.api")?.let(::startActivity)}
        button(root,"Verificar Root"){Thread{val r=RootAdapter.probeExplicitly();runOnUiThread{status.text="${r.detail}\n${capabilities()}"}}.start()}
        root.addView(TextView(this).apply{text=capabilities();textSize=13f})
        refresh()
    }
    private fun button(parent:LinearLayout,label:String,action:()->Unit){parent.addView(Button(this).apply{text=label;isAllCaps=false;setOnClickListener{action()};layoutParams=LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.WRAP_CONTENT)})}
    private fun refresh(){status.text="Estado: ${BridgeStatus.state}\n${BridgeStatus.detail}"}
    private fun requestShizuku(){
        if(!runCatching{Shizuku.pingBinder()}.getOrDefault(false)){status.text="Shizuku não está ativo";return}
        if(runCatching{Shizuku.checkSelfPermission()==PackageManager.PERMISSION_GRANTED}.getOrDefault(false)){status.text="Shizuku já autorizado";return}
        runCatching{Shizuku.requestPermission(9021)}.onFailure{status.text="Erro: ${it.message}"}
    }
    private fun capabilities():String{
        val active=runCatching{Shizuku.pingBinder()}.getOrDefault(false)
        val allowed=active&&runCatching{Shizuku.checkSelfPermission()==PackageManager.PERMISSION_GRANTED}.getOrDefault(false)
        val root=listOf("/system/bin/su","/system/xbin/su","/sbin/su").any{File(it).canExecute()}
        val termux=packageManager.getLaunchIntentForPackage("com.termux")!=null
        return "Shizuku: ${if(!active) "inativo" else if(allowed) "autorizado" else "aguardando permissão"}\nRoot: ${if(root) "su detectado (não necessariamente autorizado)" else "não detectado"}\nTermux: ${if(termux) "instalado" else "não detectado"}"
    }
}
